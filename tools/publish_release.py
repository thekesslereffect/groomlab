# SPDX-License-Identifier: Apache-2.0
"""Publish a packaged GroomLab release to the downloads bucket (Cloudflare R2).

    python tools/publish_release.py [--zip dist/GroomLab-<version>-maya-win64.zip]
                                    [--notes "..." | --notes-file notes.txt] [--dry-run]

Without --zip it runs tools/package_release.py first. The zip holds a build for every Maya it was
packaged with (GroomLab/maya_versions.json). It uploads five objects:

    maya/GroomLab-<version>-Maya-Windows.zip       the release, kept forever
    maya/GroomLab-Maya-Windows-latest.zip          the latest release
    maya/GroomLab-Maya2022-Windows-latest.zip      the same, under the name the website's "Download
                                                    free" links to (web/site.config.ts DOWNLOAD_URL)
    maya/latest.json                               {version, file, url, sha256, size, date, notes,
                                                    maya, mayaVersions, mayaBeta, platform}
    maya/releases.json                             every release, newest first: the latest.json of
                                                    each (the website's Download page lists them;
                                                    publishing a version again replaces its entry)

latest.json is what GroomLab > Check for Updates reads (groomlab/updates.py): `notes` is the short
changelog it shows (a few lines; --notes or --notes-file, empty by default), `mayaVersions` the Maya
versions the zip has a build for (an install in another Maya is not offered it), `mayaBeta` those
built but not yet tested in their Maya (Check for Updates says so), `maya` the tested one (2022): the
only field releases before mayaVersions read, and they all run in Maya 2022.

macOS (a -maya-macos-arm64.zip from package_release.py on the Mac, its binaries signed with a Developer
ID and notarized): maya/GroomLab-<version>-Maya-macOS.zip, maya/GroomLab-Maya-macOS-latest.zip,
maya/latest-macos.json (what a Mac install's Check for Updates reads) and maya/releases-macos.json, so
the Windows files above stay Windows'. Its tested versions are package_release.py's (no Maya 2022 there).

The bucket, its credentials (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, optionally
R2_BUCKET) and the signed upload: tools/downloads_bucket.py, shared with publish_blender.py and
publish_app.py.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from downloads_bucket import IMMUTABLE, PUBLIC, SHORT, json_body, read_notes, release_index, release_info, upload  # noqa: E402

SUFFIX = "-maya-win64.zip"
MAC_SUFFIX = "-maya-macos-arm64.zip"


def maya_builds(zip_path, version):
    """GroomLab/maya_versions.json of a packaged zip: {"2022": {"status": "tested"...}, ...}."""
    with zipfile.ZipFile(zip_path) as z:
        try:
            return json.loads(z.read("GroomLab-%s/GroomLab/maya_versions.json" % version).decode("utf-8"))
        except KeyError:
            sys.exit("%s has no GroomLab/maya_versions.json: package it with tools/package_release.py" % zip_path)


def check_mac_signature(zip_path, version, builds):
    """Exits unless the zip's plugins are signed with a Developer ID (not ad hoc: an unsigned or ad hoc
    build is refused on a user's Mac)."""
    import tempfile
    year = sorted(builds)[-1]
    member = "GroomLab-%s/GroomLab/%s" % (version, builds[year]["plugin"])
    with tempfile.TemporaryDirectory() as tmp, zipfile.ZipFile(zip_path) as z:
        z.extract(member, tmp)
        out = subprocess.run(["codesign", "-dv", "--verbose=2", os.path.join(tmp, member)], capture_output=True, text=True).stderr
    if "Authority=Developer ID Application" not in out:
        sys.exit("%s's plugins are not signed with a Developer ID: package it with --sign and --notarize "
                 "(tools/package_release.py, docs/MACOS.md)" % os.path.basename(zip_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--zip", help="a zip made by tools/package_release.py (default: package one now)")
    parser.add_argument("--notes", default="", help="what's new, a few lines (Check for Updates in Maya shows them)")
    parser.add_argument("--notes-file", help="the notes from a UTF-8 text file instead of --notes")
    parser.add_argument("--dry-run", action="store_true", help="show what would be uploaded, upload nothing")
    args = parser.parse_args()
    notes = read_notes(args.notes, args.notes_file)

    zip_path = args.zip
    if not zip_path:
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "package_release.py")])
        zips = glob.glob(os.path.join(ROOT, "dist", "GroomLab-*" + (MAC_SUFFIX if sys.platform == "darwin" else SUFFIX)))
        if not zips:
            sys.exit("package_release.py made no zip in dist/")
        zip_path = max(zips, key=os.path.getmtime)
    base = os.path.basename(zip_path)
    mac = base.endswith(MAC_SUFFIX)
    suffix = MAC_SUFFIX if mac else SUFFIX
    if not (base.startswith("GroomLab-") and base.endswith(suffix)):
        sys.exit("%s is not a GroomLab release zip" % base)
    version = base[len("GroomLab-"):-len(suffix)]
    builds = maya_builds(zip_path, version)
    if mac:
        check_mac_signature(zip_path, version, builds)
    elif builds.get("2022", {}).get("status") != "tested":
        sys.exit("%s has no tested Maya 2022 build" % base)
    years = sorted(builds)
    beta = [y for y in years if builds[y].get("status") != "tested"]

    with open(zip_path, "rb") as f:
        body = f.read()
    if mac:
        versioned = "maya/GroomLab-%s-Maya-macOS.zip" % version
        latest = "maya/GroomLab-Maya-macOS-latest.zip"
        info = release_info(version, versioned, body, notes, mayaVersions=years, mayaBeta=beta, platform="macos")
        uploads = [
            (versioned, body, "application/zip", IMMUTABLE),
            (latest, body, "application/zip", SHORT),
            ("maya/latest-macos.json", json_body(info), "application/json", SHORT),
            ("maya/releases-macos.json", json_body(release_index("maya/releases-macos.json", info)), "application/json", SHORT),
        ]
        if not upload(uploads, args.dry_run):
            print(json.dumps(info, indent=2))
            return
        print("GroomLab %s for macOS is live: %s/%s" % (version, PUBLIC, latest))
        return
    versioned = "maya/GroomLab-%s-Maya-Windows.zip" % version
    latest = "maya/GroomLab-Maya-Windows-latest.zip"
    info = release_info(version, versioned, body, notes, maya="2022", mayaVersions=years, mayaBeta=beta,
                        platform="windows")
    uploads = [
        (versioned, body, "application/zip", IMMUTABLE),
        (latest, body, "application/zip", SHORT),
        ("maya/GroomLab-Maya2022-Windows-latest.zip", body, "application/zip", SHORT),
        ("maya/latest.json", json_body(info), "application/json", SHORT),
        ("maya/releases.json", json_body(release_index("maya/releases.json", info)), "application/json", SHORT),
    ]
    if not upload(uploads, args.dry_run):
        print(json.dumps(info, indent=2))
        return
    print("GroomLab %s is live: %s/%s" % (version, PUBLIC, latest))


if __name__ == "__main__":
    main()
