# SPDX-License-Identifier: Apache-2.0
"""Package GroomLab for Blender: the extension zip users install with Blender's Install from Disk.

    python tools/package_blender.py [--build build/ninja-maya2022] [--config RelWithDebInfo] [--out dist]

Installs the build's Blender component (cmake --install --component Blender) into a clean staging
folder: the add-on's Python (GPL-3.0-or-later, hosts/blender/addon) with blender_manifest.toml at
the top, and bin/ with groomlab-helper, its DLLs, the presets and the helper's license and notices
(Apache-2.0, hosts/blender/helper). Checks it with Blender's own validator when Blender is found
(--blender or the newest under Program Files) and zips it to
dist/GroomLab-Blender-<version>-<platform>.zip with the manifest at the zip's root, as Blender
installs it. tools/publish_blender.py uploads it. The Maya package (package_release.py) is separate.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tomllib
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_blender():
    if sys.platform == "darwin":
        mac = "/Applications/Blender.app/Contents/MacOS/Blender"
        return mac if os.path.isfile(mac) else None
    found = glob.glob(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Blender Foundation", "Blender *", "blender.exe"))
    return sorted(found)[-1] if found else None


def manifest_of(folder):
    with open(os.path.join(folder, "blender_manifest.toml"), "rb") as f:
        return tomllib.load(f)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # macOS: the release build's helper (no TBB, macOS 13.3: docs/MACOS.md).
    parser.add_argument("--build", default=os.path.join(ROOT, "build", "macos-release" if sys.platform == "darwin" else "ninja-maya2022"))
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--blender", default=None, help="blender executable for validation (default: found)")
    # macOS: a Developer ID identity and a notarytool profile (tools/macos_sign.py); ad hoc without.
    parser.add_argument("--sign", default=os.environ.get("GROOMLAB_SIGN_IDENTITY"), help="macOS: the Developer ID Application identity")
    parser.add_argument("--notarize", default=os.environ.get("GROOMLAB_NOTARY_PROFILE"), help="macOS: the notarytool keychain profile")
    args = parser.parse_args()

    stage = os.path.join(args.out, "blender_stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    subprocess.check_call(["cmake", "--install", args.build, "--config", args.config, "--component", "Blender",
                           "--prefix", stage], stdout=subprocess.DEVNULL)
    ext = os.path.join(stage, "GroomLabBlender")
    exe = "groomlab-helper.exe" if sys.platform == "win32" else "groomlab-helper"
    for need in ("blender_manifest.toml", "__init__.py", "LICENSE", os.path.join("bin", exe), os.path.join("bin", "LICENSE"),
                 os.path.join("bin", "NOTICE"), os.path.join("bin", "THIRD_PARTY_NOTICES.txt"),
                 os.path.join("bin", "LICENSE-HELPER.txt")):
        if not os.path.isfile(os.path.join(ext, need)):
            sys.exit("no %s in the install: build %s (%s) first" % (need, args.build, args.config))
    if sys.platform == "darwin":  # the helper loads nothing from outside the extension but the system
        links = subprocess.run(["otool", "-L", os.path.join(ext, "bin", exe)], capture_output=True, text=True).stdout
        outside = [l.strip().split(" (")[0] for l in links.splitlines()[1:]
                   if not l.strip().startswith(("@rpath/", "@loader_path/", "@executable_path/", "/System/", "/usr/lib/"))]
        if outside:
            sys.exit("the helper loads from outside the extension: %s (build %s, the macos-release preset)" % (", ".join(outside), args.build))
    for folder, dirs, _ in os.walk(ext):
        for d in [d for d in dirs if d == "__pycache__"]:
            shutil.rmtree(os.path.join(folder, d))
    if sys.platform == "darwin":  # the helper signed (Developer ID with --sign, else ad hoc)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import macos_sign
        macos_sign.sign_files(ext, args.sign or "-")
        problems = macos_sign.verify_files(ext)
        if problems:
            sys.exit("signatures do not verify:\n  " + "\n  ".join(problems))
    manifest = manifest_of(ext)
    version, platform = manifest["version"], manifest["platforms"][0]

    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, "GroomLab-Blender-%s-%s.zip" % (version, platform))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for folder, _, files in os.walk(ext):
            for name in sorted(files):
                full = os.path.join(folder, name)
                z.write(full, os.path.relpath(full, ext).replace(os.sep, "/"))
    shutil.rmtree(stage)

    blender = args.blender or find_blender()
    if blender:
        run = subprocess.run([blender, "--factory-startup", "--command", "extension", "validate", zip_path],
                             capture_output=True, text=True)
        if run.returncode != 0:
            sys.exit("Blender refuses the extension:\n" + run.stdout + run.stderr)
        print("validated with %s" % blender)
    else:
        print("no Blender found: the zip is not validated")
    print("wrote %s (%.1f MB)" % (zip_path, os.path.getsize(zip_path) / 1e6))
    if sys.platform == "darwin" and args.notarize:
        if not args.sign:
            sys.exit("--notarize needs --sign (a Developer ID identity)")
        accepted, summary = macos_sign.notarize(zip_path, args.notarize)
        if not accepted:
            sys.exit("notarization refused %s: %s" % (zip_path, summary))
        print("notarized: " + summary)


if __name__ == "__main__":
    main()
