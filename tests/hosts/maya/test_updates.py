# SPDX-License-Identifier: Apache-2.0
"""Check for Updates (groomlab.updates) in mayapy, against a fake downloads site on localhost
(fake_downloads.py: latest.json and a release zip): nothing goes to the real downloads domain.

Version order; a check finding a newer release (a plain GET with GroomLab's User-Agent); an install
verifying the zip's sha256, unpacking it beside the running install, rewriting GroomLab.mod (with a
backup) and leaving the running install's files alone until the new version has started; tampered
and malformed zips refused; developer installs (no VERSION.txt, a source or build tree,
GROOMLAB_NO_UPDATES) never checked nor touched; the startup check at most once a day, off by its
preference, silent offline.

Each test has a fake packaged install (GroomLab-0.1.0/GroomLab with VERSION.txt, a modules folder
whose GroomLab.mod points at it) and a state folder of its own in a temporary folder.

Usage: mayapy test_updates.py <path/to/maya/scripts>
"""

import json
import os
import socket
import sys
import unittest

SCRIPTS_DIR = os.path.abspath(sys.argv[1])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

from groomlab import updates  # noqa: E402
from fake_downloads import ZIP_NAME, FakeDownloads, this_maya, write  # noqa: E402

# The running Maya, and others: a release for another Maya (never 2022, whose plugin a zip always
# has at its root), and two that leave this one out.
THIS = this_maya()
OTHER = "2025" if THIS != "2025" else "2024"
NOT_THIS = sorted(y for y in ("2024", "2025", "2026", "2027") if y != THIS)[:2]


def _closed_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class UpdatesTest(unittest.TestCase):
    def setUp(self):
        self.site = FakeDownloads()
        self.zip_body = self.site.release_zip()
        self.info = self.site.publish(self.zip_body)
        self.site.point(updates)
        self.module, self.mod, self.installs = self.site.module, self.site.mod, self.site.installs
        updates._announce(None)
        os.environ.pop("GROOMLAB_NO_UPDATES", None)
        if cmds.optionVar(exists=updates.STARTUP_VAR):
            cmds.optionVar(remove=updates.STARTUP_VAR)

    def tearDown(self):
        self.site.close()
        updates._announce(None)
        os.environ.pop("GROOMLAB_NO_UPDATES", None)

    def test_version_order(self):
        older = ["0.1.0", "0.1.1", "0.2.0-alpha", "0.2.0-alpha.1", "0.2.0-alpha.beta", "0.2.0-beta.2",
                 "0.2.0-beta.11", "0.2.0-rc.1", "0.2.0", "0.10.0", "1.0.0"]
        for a, b in zip(older, older[1:]):
            self.assertEqual(updates.compare_versions(a, b), -1, (a, b))
            self.assertEqual(updates.compare_versions(b, a), 1, (b, a))
        self.assertEqual(updates.compare_versions("1.2", "1.2.0"), 0)
        self.assertEqual(updates.compare_versions("v1.2.0", "1.2.0+build.5"), 0)
        self.assertIsNone(updates.parse_version("latest"))
        with self.assertRaises(ValueError):
            updates.compare_versions("1.0", "")

    def test_check_finds_a_newer_release(self):
        result = updates.check()
        self.assertTrue(result["ok"], result["message"])
        self.assertTrue(result["newer"] and result["installable"])
        self.assertEqual((result["current"], result["latest"]), ("0.1.0", "0.2.0"))
        self.assertIn("Faster brushes.", result["notes"])
        self.assertIn("GroomLab 0.2.0 is available", result["message"])
        # One plain GET of latest.json: no body, no query, GroomLab's User-Agent.
        self.assertEqual(self.site.requests, [{"method": "GET", "path": "/maya/latest.json",
                                               "agent": "GroomLab/0.1.0", "length": None}])
        # The same version: nothing to offer.
        self.site.publish(self.zip_body, version="0.1.0")
        result = updates.check()
        self.assertTrue(result["ok"])
        self.assertFalse(result["newer"] or result["installable"])
        self.assertIn("latest version", result["message"])
        # A release for another Maya: said, not offered.
        self.site.publish(self.zip_body, maya=OTHER)
        result = updates.check()
        self.assertTrue(result["newer"])
        self.assertFalse(result["installable"])
        self.assertIn("Maya " + OTHER, result["message"])
        self.assertIn("Maya " + OTHER, updates.install(result["info"])["message"])

    def test_one_release_for_every_maya(self):
        # A zip with a build for several Mayas (mayaVersions): offered in each of them, the betas said.
        self.site.publish(self.zip_body, mayaVersions=sorted([THIS, OTHER]), mayaBeta=[OTHER])
        result = updates.check()
        self.assertTrue(result["installable"], result["message"])
        self.assertNotIn("beta", result["message"])  # this Maya's build is the tested one
        self.assertEqual(updates.release_mayas(result["info"]), sorted([THIS, OTHER]))
        self.assertTrue(updates.beta_for(result["info"], int(OTHER)))
        self.assertFalse(updates.beta_for(result["info"], int(THIS)))
        # A release whose builds leave this Maya out: said, not offered (maya, what releases before
        # mayaVersions named alone, is not read when mayaVersions is there).
        self.site.publish(self.zip_body, mayaVersions=NOT_THIS)
        result = updates.check()
        self.assertTrue(result["newer"])
        self.assertFalse(result["installable"])
        self.assertIn("Maya " + ", ".join(NOT_THIS), result["message"])
        self.assertIn("Maya " + ", ".join(NOT_THIS), updates.install(result["info"])["message"])
        self.assertEqual(updates.release_mayas({"maya": "2022"}), ["2022"])
        self.assertEqual(updates.release_mayas({}), [])
        # A zip without the running Maya's build is refused when it is unpacked.
        zip_path = os.path.join(self.site.root, "check.zip")
        with open(zip_path, "wb") as f:
            f.write(self.zip_body)
        with self.assertRaises(updates.UpdateError) as caught:
            updates._unpack(zip_path, os.path.join(self.site.root, "unpacked-other"), "0.2.0", int(OTHER))
        self.assertIn("no GroomLab for Maya " + OTHER, str(caught.exception))
        with open(zip_path, "wb") as f:
            f.write(self.site.release_zip(extra={"GroomLab-0.2.0/GroomLab/maya%s/plug-ins/%s" % (OTHER, updates.PLUGIN_FILE): OTHER}))
        self.assertTrue(os.path.isdir(updates._unpack(zip_path, os.path.join(self.site.root, "unpacked"), "0.2.0",
                                                      int(OTHER))))
        # A macOS zip: every build in maya<year>/, none at the module's root (no Maya 2022): it
        # installs in the Mayas it has a build for (0.47.0's was refused: "no GroomLab module").
        import zipfile
        with zipfile.ZipFile(zip_path, "w") as z:
            top = "GroomLab-0.2.0/"
            z.writestr(top + "GroomLab/maya%s/plug-ins/%s" % (OTHER, updates.PLUGIN_FILE), "the new plugin")
            z.writestr(top + "GroomLab/VERSION.txt", "GroomLab 0.2.0 (def5678, RelWithDebInfo)\n")
            z.writestr(top + "GroomLab.mod", "+ MAYAVERSION:%s GroomLab 0.2.0 GroomLab/maya%s\n" % (OTHER, OTHER))
        for n, maya in enumerate((int(OTHER), 0)):
            self.assertTrue(os.path.isdir(updates._unpack(zip_path, os.path.join(self.site.root, "unpacked-mac%d" % n),
                                                          "0.2.0", maya)))
        with self.assertRaises(updates.UpdateError) as caught:
            updates._unpack(zip_path, os.path.join(self.site.root, "unpacked-mac-2022"), "0.2.0", 2022)
        self.assertIn("no GroomLab for Maya 2022", str(caught.exception))

    def test_install_unpacks_beside_and_rewrites_the_mod(self):
        result = updates.check()
        seen = []
        installed = updates.install(result["info"], progress=lambda done, total: seen.append((done, total)))
        self.assertTrue(installed["ok"], installed["message"])
        self.assertIn("Restart Maya to finish", installed["message"])
        new_module = self.installs + "/GroomLab-0.2.0/GroomLab"
        self.assertEqual(installed["folder"], self.installs + "/GroomLab-0.2.0")
        self.assertEqual(updates.installed_version(new_module), "0.2.0")
        self.assertEqual(seen[-1], (len(self.zip_body), len(self.zip_body)))
        # GroomLab.mod points at the new module (absolute; the bundled .mod's other lines kept); the
        # old one is GroomLab.mod.bak; nothing else is left in the modules folder.
        with open(self.mod) as f:
            text = f.read()
        self.assertEqual(text, "+ MAYAVERSION:%s GroomLab 0.2.0 %s\nPYTHONPATH+:=python\n"
                               "MTOA_EXTENSIONS_PATH+:=extensions\n" % (THIS, new_module))
        with open(self.mod + ".bak") as f:
            self.assertEqual(f.read(), self.site.mod_text)
        self.assertEqual(updates.find_mod_file(new_module), self.mod)
        self.assertEqual(sorted(os.listdir(self.site.modules)), ["GroomLab.mod", "GroomLab.mod.bak"])
        # The running install is untouched (Windows locks its .mll); no staging folder left.
        self.assertEqual(self.site.installed_folders(), ["GroomLab-0.1.0", "GroomLab-0.2.0"])
        with open(self.module + "/plug-ins/" + updates.PLUGIN_FILE) as f:
            self.assertEqual(f.read(), "the running plugin")
        # Until Maya restarts: the check says so, and a second install is refused.
        again = updates.check()
        self.assertTrue(again["restart"])
        self.assertFalse(again["installable"])
        self.assertIn("Restart Maya to finish", again["message"])
        self.assertFalse(updates.install(again["info"])["ok"])
        # A start still on the old version (not restarted, or gone back): everything stays.
        self.assertEqual(updates.finish_pending(self.module), "")
        self.assertEqual(self.site.installed_folders(), ["GroomLab-0.1.0", "GroomLab-0.2.0"])
        # The new version's first start: the previous folder goes, the state forgets the update.
        self.assertEqual(updates.finish_pending(new_module), self.installs + "/GroomLab-0.1.0")
        self.assertEqual(self.site.installed_folders(), ["GroomLab-0.2.0"])
        self.assertNotIn("pending", updates.read_state())
        self.assertEqual(updates.finish_pending(new_module), "")

    def assert_nothing_installed(self):
        self.assertEqual(self.site.installed_folders(), ["GroomLab-0.1.0"])
        with open(self.mod) as f:
            self.assertEqual(f.read(), self.site.mod_text)
        self.assertEqual(os.listdir(self.site.modules), ["GroomLab.mod"])
        self.assertNotIn("pending", updates.read_state())

    def test_tampered_zip_is_refused(self):
        # latest.json's checksum is the real release's; the zip on the server was changed.
        tampered = self.site.release_zip(extra={"GroomLab-0.2.0/GroomLab/scripts/evil.py": "print('hi')"})
        self.site.replace_zip(tampered)
        info = dict(self.info, size=len(tampered))  # the size alone proves nothing
        result = updates.install(info)
        self.assertFalse(result["ok"])
        self.assertIn("checksum does not match", result["message"])
        self.assert_nothing_installed()
        # A zip whose checksum matches but whose files would land outside its folder: refused too.
        self.site.publish(self.site.release_zip(extra={"GroomLab-0.2.0/../../escaped.txt": "x"}))
        result = updates.install(self.site.info)
        self.assertFalse(result["ok"])
        self.assertIn("unexpected file", result["message"])
        self.assertFalse(os.path.exists(os.path.join(self.site.root, "escaped.txt")))
        self.assert_nothing_installed()
        # Not a zip at all, and a release whose download is on another host.
        self.site.publish(b"not a zip")
        self.assertIn("not a zip", updates.install(self.site.info)["message"])
        info = dict(self.site.info, url="https://elsewhere.example.com/" + ZIP_NAME)
        self.assertIn("is not on", updates.install(info)["message"])
        self.assert_nothing_installed()

    def test_developer_installs_are_never_touched(self):
        # No VERSION.txt (cmake --install, as the developer's own Documents/maya/modules install).
        os.remove(self.module + "/VERSION.txt")
        result = updates.check()
        self.assertTrue(result["off"])
        self.assertEqual(result["message"], "Updates are off for this install (developer build)")
        result = updates.install(self.info)
        self.assertFalse(result["ok"])
        self.assertEqual(result["message"], updates.DEV_MESSAGE)
        self.assertIsNone(updates.startup(now=1e9))
        self.assertEqual(self.site.requests, [])  # nothing asked
        self.assert_nothing_installed()
        # A packaged-looking module inside a source checkout or a build folder.
        write(self.module + "/VERSION.txt", "GroomLab 0.1.0 (abc1234, RelWithDebInfo)\n")
        self.assertEqual(updates.off_reason(), "")
        for marker in ("CMakeCache.txt", "CMakeLists.txt"):
            write(self.installs + "/" + marker, "")
            self.assertEqual(updates.off_reason(), updates.DEV_MESSAGE, marker)
            os.remove(self.installs + "/" + marker)
        # GROOMLAB_NO_UPDATES (a studio's machines).
        os.environ["GROOMLAB_NO_UPDATES"] = "1"
        self.assertTrue(updates.check()["off"])
        self.assertIn("GROOMLAB_NO_UPDATES", updates.install(self.info)["message"])
        del os.environ["GROOMLAB_NO_UPDATES"]
        # The running module here is the source tree (hosts/maya): a developer build.
        updates.MODULE_DIR = None
        self.assertEqual(updates.off_reason(), updates.DEV_MESSAGE)
        self.assertEqual(self.site.requests, [])
        self.assert_nothing_installed()

    def finish_background(self, thread):
        thread.join(10)  # batch: the answer is applied on the thread
        self.assertFalse(thread.is_alive())

    def test_startup_check_once_a_day(self):
        day = 24 * 3600
        now = 1.8e9
        thread = updates.startup(now=now)
        self.assertIsNotNone(thread)
        self.finish_background(thread)
        self.assertEqual(len(self.site.requests), 1)
        found = updates.available()
        self.assertTrue(found and found["latest"] == "0.2.0")
        self.assertEqual(updates.note_text(), "GroomLab 0.2.0 is available — GroomLab > Help > Check for Updates")
        # The same day: no second request, nor an hour later.
        self.assertIsNone(updates.startup(now=now + 3600))
        self.assertEqual(len(self.site.requests), 1)
        # The next day: again.
        self.finish_background(updates.startup(now=now + day + 1))
        self.assertEqual(len(self.site.requests), 2)
        # Turned off (the window's checkbox): never.
        updates.set_startup_check(False)
        self.assertFalse(updates.startup_check_enabled())
        self.assertIsNone(updates.startup(now=now + 3 * day))
        self.assertEqual(len(self.site.requests), 2)
        updates.set_startup_check(True)
        # The session's own start (on_startup): never in batch, which mayapy is.
        updates._started = False
        self.assertIsNone(updates.on_startup(now=now + 5 * day))
        self.assertEqual(len(self.site.requests), 2)

    def test_https_verified_with_the_systems_certificates(self):
        # A Python that found no trusted certificates of its own (Maya's on macOS): the system's
        # bundle loaded, the server still verified.
        import ssl
        import tempfile
        own = ssl.create_default_context()
        pems = [ssl.DER_cert_to_PEM_cert(der) for der in own.get_ca_certs(binary_form=True)]
        if not pems:
            own.load_default_certs()
            pems = [ssl.DER_cert_to_PEM_cert(der) for der in own.get_ca_certs(binary_form=True)]
        if not pems:
            self.skipTest("no trusted certificates on this machine to stand in for a system bundle")
        folder = tempfile.mkdtemp(prefix="groomlab_ca_")
        bundle = os.path.join(folder, "cert.pem")
        with open(bundle, "w") as f:
            f.write("".join(pems))
        create, bundles = ssl.create_default_context, updates.CA_BUNDLES
        ssl.create_default_context = lambda *a, **k: ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)  # none of its own
        updates.CA_BUNDLES = (os.path.join(folder, "missing.pem"), bundle)
        try:
            context = updates.ssl_context()
        finally:
            ssl.create_default_context, updates.CA_BUNDLES = create, bundles
        self.assertGreater(context.cert_store_stats()["x509_ca"], 0)
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(context.check_hostname)
        # The default: verified too, and the check still reaches the (plain http, local) site.
        context = updates.ssl_context()
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(updates.check()["reached"])

    def test_offline_is_silent(self):
        updates.LATEST_URL = "http://127.0.0.1:%d/maya/latest.json" % _closed_port()
        result = updates.check(timeout=2)
        self.assertFalse(result["ok"] or result["reached"] or result["newer"])
        self.assertIn("Couldn't reach 127.0.0.1", result["message"])
        heard = []
        updates.on_available(heard.append)
        try:
            self.finish_background(updates.startup(now=2e9))
        finally:
            updates.remove_listener(heard.append)
        self.assertIsNone(updates.available())
        self.assertEqual(heard, [])
        # A server answering with something that is not a release: reached, nothing offered.
        updates.LATEST_URL = self.site.url + "/maya/nothing.json"
        result = updates.check()
        self.assertFalse(result["ok"] or result["newer"])
        self.assertTrue(result["reached"])
        self.assertIn("HTTP 404", result["message"])
        self.site.write_latest("<html>not json</html>")
        updates.LATEST_URL = self.site.url + "/maya/latest.json"
        result = updates.check()
        self.assertFalse(result["ok"])
        self.assertTrue(result["reached"])
        self.site.write_latest(json.dumps(dict(self.info, sha256="")))
        self.assertIn("no download or checksum", updates.check()["message"])


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
