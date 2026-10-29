# SPDX-License-Identifier: Apache-2.0
"""The add-on's updater (groomlab/updates.py): off without online access, a check against a
release served on localhost (newer, current, a bad checksum, another platform), Help > Check for
Updates bound to its dialog, and Update installing the served zip over the running extension (the
helper stopped first), the add-on enabled again."""

import hashlib
import http.server
import importlib
import json
import os
import shutil
import sys
import threading
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

pkg, m = bu.enable()
process = m["process"]
updates = importlib.import_module(bu.MODULE + ".updates")
actions = importlib.import_module(bu.MODULE + ".actions")
ZIP = os.environ.get("GROOMLAB_EXTENSION_ZIP", "")
OFF_REASON = updates.off_reason  # the real one (the tests stand in for online access)


class Server(object):
    """A folder served over http on localhost, in a thread."""

    def __init__(self, folder):
        handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=folder, **k)  # noqa: E731
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.url = "http://127.0.0.1:%d/" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


class UpdatesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = os.path.join(bu.TMP, "release")
        shutil.rmtree(cls.folder, ignore_errors=True)
        os.makedirs(cls.folder)
        cls.server = Server(cls.folder)
        updates.off_reason = lambda: ""  # (background Blender has no online access: the rest is tested)

    @classmethod
    def tearDownClass(cls):
        updates.off_reason = OFF_REASON
        cls.server.close()

    def release(self, version, sha=None, platform=None, size=None):
        """latest.json for `version`, its zip the built extension (served as release.zip)."""
        zip_path = os.path.join(self.folder, "release.zip")
        if not os.path.isfile(zip_path):
            shutil.copyfile(ZIP, zip_path)
        data = open(zip_path, "rb").read()
        info = {"version": version, "url": self.server.url + "release.zip", "size": size or len(data),
                "sha256": sha or hashlib.sha256(data).hexdigest(), "notes": "- A test release.",
                "blender": "5.0.0", "platform": platform or updates.platform()}
        with open(os.path.join(self.folder, "latest.json"), "w", encoding="utf-8") as f:
            json.dump(info, f)
        return self.server.url + "latest.json"

    def test_1_off_without_online_access_and_bound(self):
        self.assertTrue(OFF_REASON())  # background mode: Blender's online access is off
        self.assertFalse(updates.check_due())  # never at startup in background mode
        self.assertEqual(updates.repo_module(), "user_default")
        self.assertTrue(updates.current_version())
        self.assertEqual(actions.BINDINGS["help.updates"][0], "groomlab.check_updates")
        self.assertTrue(hasattr(bpy.types, "GROOMLAB_OT_check_updates"))

    def test_2_check(self):
        current = updates.current_version()
        r = updates.check(url=self.release("99.0.0"))
        self.assertTrue(r["ok"] and r["newer"] and r["installable"], r)
        self.assertEqual((r["current"], r["latest"]), (current, "99.0.0"))
        self.assertEqual(r["notes"], "- A test release.")
        r = updates.check(url=self.release(current))
        self.assertTrue(r["ok"] and not r["newer"] and not r["installable"], r)
        self.assertIn("latest version", r["message"])
        r = updates.check(url=self.release("99.0.0", platform="beos-x64"))
        self.assertFalse(r["installable"])
        self.assertIn("beos-x64", r["message"])
        r = updates.check(url=self.server.url + "nothing.json")
        self.assertFalse(r["ok"])
        self.assertTrue(r["reached"])  # the server answered (404)
        # A download that is not what latest.json says is refused.
        latest = self.release("99.0.0", sha="0" * 64)
        r = updates.check(url=latest)
        out = updates.install(r["info"], latest_url=latest)
        self.assertFalse(out["ok"])
        self.assertIn("checksum", out["message"])
        with self.assertRaises(updates.UpdateError):  # from somewhere else
            updates.download(dict(r["info"], url="https://example.com/x.zip"), latest_url=latest)

    def test_3_update_installs_over_this_one(self):
        latest = self.release("99.0.0")
        r = updates.check(url=latest)
        self.assertTrue(r["installable"])
        process.client()  # the helper runs: Update stops it before installing
        self.assertTrue(process.running())
        out = updates.install(r["info"], latest_url=latest)
        self.assertTrue(out["ok"], out)
        self.assertIn("Restart Blender", out["message"])
        self.assertFalse(process.running())
        self.assertIn(bu.MODULE, bpy.context.preferences.addons)  # enabled again
        self.assertTrue(os.path.isfile(os.path.join(updates.addon_dir(), "blender_manifest.toml")))


if __name__ == "__main__":
    bu.run(UpdatesTest)
