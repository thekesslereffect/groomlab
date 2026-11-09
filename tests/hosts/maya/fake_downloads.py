# SPDX-License-Identifier: Apache-2.0
"""A fake GroomLab downloads site for the update tests (test_updates.py, gui_updates.py): an
http.server on 127.0.0.1 serving maya/latest.json and a release zip from a temporary folder, beside a
fake packaged install (GroomLab-0.1.0/GroomLab with VERSION.txt, a modules folder whose GroomLab.mod
points at it). Nothing goes to the real downloads domain. The install and the release are the
running Maya's (site.maya: Maya 2022's plugin at the module's root, any other's in maya<year>/, as
tools/package_release.py lays them out).

    site = FakeDownloads()            # .root, .module, .modules, .mod, .mod_text, .state, .url, .requests
    site.publish(site.release_zip())  # latest.json for GroomLab 0.2.0 (site.info)
    site.point(updates)               # groomlab.updates reads this install and this server
    site.close()                      # the server stops, the folder goes, updates as it was
"""

import functools
import hashlib
import json
import os
import shutil
import sys
import tempfile
import threading
import zipfile

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

# The plugin's file in a module (groomlab.updates.PLUGIN_FILE).
PLUGIN_FILE = "GroomLab.bundle" if sys.platform == "darwin" else "GroomLab.mll"

ZIP_NAME = "GroomLab-0.2.0-Maya2022-Windows.zip"


def this_maya():
    """The running Maya's year as a string ("2022" when it can't be told)."""
    from groomlab import updates
    return str(updates.maya_major() or 2022)


def write(path, text):
    folder = os.path.dirname(path)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    with open(path, "w") as f:
        f.write(text)


class FakeDownloads(object):
    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="groomlab_updates_").replace("\\", "/")
        self.maya = this_maya()
        # The install: a path with a space in it, as a user's Documents folder may have.
        self.installs = self.root + "/My Tools"
        self.module = self.installs + "/GroomLab-0.1.0/GroomLab"
        write(self.module + "/VERSION.txt", "GroomLab 0.1.0 (abc1234, RelWithDebInfo)\n")
        write(self.module + "/plug-ins/" + PLUGIN_FILE, "the running plugin")
        self.modules = self.root + "/modules"
        self.mod = self.modules + "/GroomLab.mod"
        self.mod_text = "+ MAYAVERSION:%s GroomLab 0.1.0 %s\nPYTHONPATH+:=python\n" % (self.maya, self.module)
        write(self.mod, self.mod_text)
        self.state = self.root + "/state"
        self.site = self.root + "/site"
        os.makedirs(self.site + "/maya")
        self.requests = []
        owner = self

        class Handler(SimpleHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append({"method": "GET", "path": self.path, "agent": self.headers.get("User-Agent"),
                                       "length": self.headers.get("Content-Length")})
                return SimpleHTTPRequestHandler.do_GET(self)

            def do_POST(self):
                owner.requests.append({"method": "POST", "path": self.path})
                self.send_error(405)

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=self.site))
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        self.updates = None

    def release_zip(self, version="0.2.0", extra=None):
        """A release zip's bytes, laid out as tools/package_release.py makes it."""
        path = os.path.join(self.root, "release.zip")
        top = "GroomLab-%s/" % version
        with zipfile.ZipFile(path, "w") as z:
            z.writestr(top + "GroomLab/plug-ins/" + PLUGIN_FILE, "the new plugin")
            if self.maya != "2022":  # the running Maya's build
                z.writestr(top + "GroomLab/maya%s/plug-ins/%s" % (self.maya, PLUGIN_FILE), "the new plugin")
            z.writestr(top + "GroomLab/VERSION.txt", "GroomLab %s (def5678, RelWithDebInfo)\n" % version)
            z.writestr(top + "GroomLab/scripts/groomlab/__init__.py", "")
            z.writestr(top + "GroomLab.mod", "+ MAYAVERSION:%s GroomLab %s GroomLab\nPYTHONPATH+:=python\n"
                       "MTOA_EXTENSIONS_PATH+:=extensions\n" % (self.maya, version))
            z.writestr(top + "install.mel", "// installer\n")
            for name, data in (extra or {}).items():
                z.writestr(name, data)
        with open(path, "rb") as f:
            body = f.read()
        os.remove(path)
        return body

    def publish(self, body, version="0.2.0", sha256=None, notes="Faster brushes.\nThe Graph Editor remembers its zoom.",
                maya=None, **extra):
        """Serves body as the release zip and a latest.json naming it (self.info; `maya` the running
        Maya's by default; `extra`: more fields, mayaVersions and mayaBeta)."""
        with open(os.path.join(self.site, "maya", ZIP_NAME), "wb") as f:
            f.write(body)
        self.info = {"version": version, "file": "maya/" + ZIP_NAME, "url": self.url + "/maya/" + ZIP_NAME,
                     "sha256": sha256 or hashlib.sha256(body).hexdigest(), "size": len(body),
                     "date": "2026-10-05T00:00:00Z", "notes": notes, "maya": maya or self.maya, "platform": "windows"}
        self.info.update(extra)
        self.write_latest(json.dumps(self.info))
        return self.info

    def write_latest(self, text):
        write(os.path.join(self.site, "maya", "latest.json"), text)

    def replace_zip(self, body):
        """The zip on the server changed after latest.json was written (tampered)."""
        with open(os.path.join(self.site, "maya", ZIP_NAME), "wb") as f:
            f.write(body)

    def installed_folders(self):
        return sorted(os.listdir(self.installs))

    def point(self, updates):
        """groomlab.updates reads this install, its modules folder, a state folder and this server."""
        self.updates = updates
        self.real_latest_url = updates.LATEST_URL  # (this platform's feed, put back by close)
        updates.MODULE_DIR = self.module
        updates.MOD_DIRS = [self.modules]
        updates.STATE_DIR = self.state
        updates.LATEST_URL = self.url + "/maya/latest.json"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        if self.updates is not None:
            self.updates.MODULE_DIR = self.updates.MOD_DIRS = self.updates.STATE_DIR = None
            self.updates.LATEST_URL = self.real_latest_url
        shutil.rmtree(self.root, ignore_errors=True)
