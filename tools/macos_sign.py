# SPDX-License-Identifier: Apache-2.0
"""Signing and notarization for the macOS packages (package_release.py, package_app.py,
package_blender.py: their --sign and --notarize).

    --sign "Developer ID Application: <name> (<team>)"   or GROOMLAB_SIGN_IDENTITY
    --notarize <notarytool keychain profile>              or GROOMLAB_NOTARY_PROFILE

The identity is a Developer ID Application certificate in the login keychain (security
find-identity -v -p codesigning); the profile is one `xcrun notarytool store-credentials` saved.
Neither is in the repository. Without --sign a package is signed ad hoc (it runs on the Mac that
built it alone). Every Mach-O is signed with the hardened runtime and a secure timestamp, as
notarization requires, from the inside out: a bundle's nested code before the bundle.
"""

import json
import os
import subprocess

MACHO_MAGICS = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        return f.read(4) in MACHO_MAGICS


def _codesign(path, identity, entitlements=None):
    cmd = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        cmd += ["--options", "runtime", "--timestamp"]
    if entitlements:
        cmd += ["--entitlements", entitlements]
    subprocess.check_call(cmd + [path], stderr=subprocess.DEVNULL)


def sign_files(root, identity):
    """Signs every Mach-O file under `root` (a Maya module, an extension folder): the plugin
    bundles, dylibs and executables there are single files. Returns how many."""
    signed = 0
    for folder, _, files in os.walk(root):
        for name in sorted(files):
            path = os.path.join(folder, name)
            if is_macho(path):
                _codesign(path, identity)
                signed += 1
    return signed


def sign_app(app, identity, entitlements=None):
    """Signs a .app inside out: the Mach-O files in Contents/PlugIns and loose ones in
    Contents/Frameworks, each framework as a bundle, then the app itself."""
    contents = os.path.join(app, "Contents")
    for sub in ("PlugIns", "Frameworks"):
        top = os.path.join(contents, sub)
        for folder, dirs, files in os.walk(top):
            dirs[:] = [d for d in dirs if not d.endswith(".framework")]  # frameworks: signed whole, below
            for name in sorted(files):
                path = os.path.join(folder, name)
                if is_macho(path):
                    _codesign(path, identity)
    frameworks = os.path.join(contents, "Frameworks")
    if os.path.isdir(frameworks):
        for name in sorted(os.listdir(frameworks)):
            if name.endswith(".framework"):
                _codesign(os.path.join(frameworks, name), identity)
    _codesign(app, identity, entitlements)
    subprocess.check_call(["codesign", "--verify", "--deep", "--strict", app])


def verify_files(root):
    """Problems: a Mach-O under `root` whose signature does not verify strictly."""
    problems = []
    for folder, _, files in os.walk(root):
        for name in files:
            path = os.path.join(folder, name)
            if is_macho(path):
                run = subprocess.run(["codesign", "--verify", "--strict", path], capture_output=True, text=True)
                if run.returncode != 0:
                    problems.append("%s: %s" % (os.path.relpath(path, root), run.stderr.strip()))
    return problems


def notarize(zip_path, profile):
    """Submits `zip_path` to Apple's notary service and waits; returns (accepted, summary). On a
    rejection the summary holds the notary log's issues."""
    run = subprocess.run(["xcrun", "notarytool", "submit", zip_path, "--keychain-profile", profile, "--wait",
                          "--output-format", "json"], capture_output=True, text=True)
    try:
        result = json.loads(run.stdout)
    except ValueError:
        return False, (run.stdout + run.stderr).strip()
    status = result.get("status", "")
    if status == "Accepted":
        return True, "accepted (submission %s)" % result.get("id", "?")
    log = subprocess.run(["xcrun", "notarytool", "log", result.get("id", ""), "--keychain-profile", profile],
                         capture_output=True, text=True).stdout
    return False, "%s (submission %s)\n%s" % (status or "failed", result.get("id", "?"), log[-4000:])


def staple(path):
    subprocess.check_call(["xcrun", "stapler", "staple", path], stdout=subprocess.DEVNULL)
