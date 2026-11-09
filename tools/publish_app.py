# SPDX-License-Identifier: Apache-2.0
"""Publish the standalone GroomLab app (the Windows beta) to the downloads bucket (Cloudflare R2).

    python tools/publish_app.py [--zip dist/GroomLab-App-<version>-beta-windows-x64.zip]
                                [--notes "..." | --notes-file notes.txt] [--dry-run]

Without --zip it runs tools/package_app.py first (which smoke-runs the zip from a clean folder).
It uploads four objects:

    app/GroomLab-App-<version>-beta-windows-x64.zip   the release, kept forever
    app/GroomLab-App-windows-latest.zip               what the website's app download links to
    app/latest.json                                   {version, file, url, sha256, size, date, notes,
                                                       channel, platform}: Help > Check for Updates
                                                       in the app reads it (update_check.h)
    app/releases.json                                 every release, newest first (the website's
                                                       Download page lists them; publishing a
                                                       version again replaces its entry)

macOS (a -beta-macos-arm64.zip from package_app.py on the Mac: GroomLab.app signed with a Developer ID,
notarized and stapled): app/GroomLab-App-<version>-beta-macos-arm64.zip, app/GroomLab-App-macos-latest.zip,
app/latest-macos.json (what the Mac app's Check for Updates reads) and app/releases-macos.json, so the
Windows files above stay Windows'.

The bucket, its credentials (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, optionally
R2_BUCKET) and the signed upload: tools/downloads_bucket.py, as for the Maya release.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from downloads_bucket import IMMUTABLE, PUBLIC, SHORT, json_body, read_notes, release_index, release_info, upload  # noqa: E402

ZIP_NAME = re.compile(r"GroomLab-App-(?P<version>[0-9][0-9A-Za-z.]*)-(?P<channel>beta)-(?P<platform>windows-x64|macos-arm64)\.zip")


def check_mac_app(zip_path, version, channel):
    """Exits unless the zip's GroomLab.app is signed with a Developer ID and its notarization ticket is
    stapled (what Gatekeeper on a user's Mac accepts without asking)."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.check_call(["ditto", "-x", "-k", zip_path, tmp])
        app = os.path.join(tmp, "GroomLab-App-%s-%s" % (version, channel), "GroomLab.app")
        out = subprocess.run(["codesign", "-dv", "--verbose=2", app], capture_output=True, text=True).stderr
        stapled = subprocess.run(["xcrun", "stapler", "validate", app], capture_output=True, text=True).returncode == 0
    if "Authority=Developer ID Application" not in out or not stapled:
        sys.exit("%s's GroomLab.app is not signed with a Developer ID and notarized: package it with --sign and "
                 "--notarize (tools/package_app.py, docs/MACOS.md)" % os.path.basename(zip_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--zip", help="a zip made by tools/package_app.py (default: package one now)")
    parser.add_argument("--notes", default="", help="what's new, a few lines (Check for Updates in the app shows them)")
    parser.add_argument("--notes-file", help="the notes from a UTF-8 text file instead of --notes")
    parser.add_argument("--dry-run", action="store_true", help="show what would be uploaded, upload nothing")
    args = parser.parse_args()
    notes = read_notes(args.notes, args.notes_file)

    zip_path = args.zip
    if not zip_path:
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "package_app.py")])
        zips = glob.glob(os.path.join(ROOT, "dist", "GroomLab-App-*-%s.zip" % ("macos-arm64" if sys.platform == "darwin" else "windows-x64")))
        if not zips:
            sys.exit("package_app.py made no zip in dist/")
        zip_path = max(zips, key=os.path.getmtime)
    m = ZIP_NAME.fullmatch(os.path.basename(zip_path))
    if not m:
        sys.exit("%s is not a GroomLab app zip (GroomLab-App-<version>-beta-windows-x64.zip or -macos-arm64.zip)" % os.path.basename(zip_path))
    version, channel, platform = m.group("version", "channel", "platform")
    with open(zip_path, "rb") as f:
        body = f.read()
    mac = platform == "macos-arm64"
    exe = "GroomLab.app/Contents/MacOS/GroomLab" if mac else "GroomLab.exe"
    with zipfile.ZipFile(zip_path) as z:
        if "GroomLab-App-%s-%s/%s" % (version, channel, exe) not in z.namelist():
            sys.exit("%s holds no GroomLab-App-%s-%s/%s" % (zip_path, version, channel, exe))
    if mac:
        check_mac_app(zip_path, version, channel)

    versioned = "app/GroomLab-App-%s-%s-%s.zip" % (version, channel, platform)
    latest = "app/GroomLab-App-%s-latest.zip" % ("macos" if mac else "windows")
    feed, index = ("app/latest-macos.json", "app/releases-macos.json") if mac else ("app/latest.json", "app/releases.json")
    info = release_info(version, versioned, body, notes, channel=channel, platform=platform)
    uploads = [
        (versioned, body, "application/zip", IMMUTABLE),
        (latest, body, "application/zip", SHORT),
        (feed, json_body(info), "application/json", SHORT),
        (index, json_body(release_index(index, info)), "application/json", SHORT),
    ]
    if not upload(uploads, args.dry_run):
        print(json.dumps(info, indent=2))
        return
    print("GroomLab app %s (%s) is live: %s/%s" % (version, channel, PUBLIC, latest))


if __name__ == "__main__":
    main()
