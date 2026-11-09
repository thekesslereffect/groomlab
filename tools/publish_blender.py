# SPDX-License-Identifier: Apache-2.0
"""Publish the GroomLab Blender extension to the downloads bucket (Cloudflare R2), beside Maya's.

    python tools/publish_blender.py [--zip dist/GroomLab-Blender-<version>-windows-x64.zip]
                                    [--notes "..." | --notes-file notes.txt] [--dry-run]

Without --zip it runs tools/package_blender.py first. It uploads five objects:

    blender/GroomLab-Blender-<version>-windows-x64.zip   the release, kept forever
    blender/GroomLab-Blender-windows-x64-latest.zip      what the website's Blender download links to
    blender/latest.json                                   {version, file, url, sha256, size, date, notes,
                                                           blender (the oldest Blender), platform}
    blender/index.json                                    a Blender extensions repository listing (API v1):
                                                           add https://downloads.getgroomlab.com/blender/index.json
                                                           as a remote repository and Blender offers updates
    blender/releases.json                                 every release, newest first: the latest.json of
                                                           each (the website's Download page lists them)

The bucket, its credentials (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, optionally
R2_BUCKET) and the signed upload: tools/downloads_bucket.py, as for the Maya release.
"""

import argparse
import glob
import hashlib
import io
import json
import os
import subprocess
import sys
import tomllib
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from downloads_bucket import (IMMUTABLE, PUBLIC, SHORT, json_body, published_json, read_notes, release_index,  # noqa: E402
                              release_info, upload)


def repository_entry(manifest, url, body):
    """The extension as Blender's server-generate lists it: the manifest's fields (no build
    settings) plus the archive's URL, size and hash."""
    entry = {k: v for k, v in manifest.items() if k not in ("build", "wheels") and v is not None}
    entry["archive_url"] = url
    entry["archive_size"] = len(body)
    entry["archive_hash"] = "sha256:" + hashlib.sha256(body).hexdigest()
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--zip", help="a zip made by tools/package_blender.py (default: package one now)")
    parser.add_argument("--notes", default="", help="what's new, a few lines")
    parser.add_argument("--notes-file", help="the notes from a UTF-8 text file instead of --notes")
    parser.add_argument("--dry-run", action="store_true", help="show what would be uploaded, upload nothing")
    args = parser.parse_args()
    notes = read_notes(args.notes, args.notes_file)

    zip_path = args.zip
    if not zip_path:
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "package_blender.py")])
        zips = glob.glob(os.path.join(ROOT, "dist", "GroomLab-Blender-*.zip"))
        if not zips:
            sys.exit("package_blender.py made no zip in dist/")
        zip_path = max(zips, key=os.path.getmtime)
    with open(zip_path, "rb") as f:
        body = f.read()
    with zipfile.ZipFile(io.BytesIO(body)) as z:
        manifest = tomllib.loads(z.read("blender_manifest.toml").decode("utf-8"))
    if manifest.get("id") != "groomlab":
        sys.exit("%s is not the GroomLab extension" % zip_path)
    version, platform = manifest["version"], manifest["platforms"][0]

    versioned = "blender/GroomLab-Blender-%s-%s.zip" % (version, platform)
    latest = "blender/GroomLab-Blender-%s-latest.zip" % platform
    info = release_info(version, versioned, body, notes, blender=manifest.get("blender_version_min", ""), platform=platform)
    # Blender's repository lists one entry per platform: this platform's replaced, the others' kept.
    entry = repository_entry(manifest, info["url"], body)
    others = [e for e in (published_json("blender/index.json") or {}).get("data", [])
              if platform not in (e.get("platforms") or [])]
    index = {"version": "v1", "blocklist": [], "data": others + [entry]}
    # Each platform's own feed and release list (macOS': latest-macos.json, releases-macos.json).
    mac = platform.startswith("macos")
    feed, releases = ("blender/latest-macos.json", "blender/releases-macos.json") if mac else ("blender/latest.json", "blender/releases.json")
    uploads = [
        (versioned, body, "application/zip", IMMUTABLE),
        (latest, body, "application/zip", SHORT),
        (feed, json_body(info), "application/json", SHORT),
        ("blender/index.json", json_body(index), "application/json", SHORT),
        (releases, json_body(release_index(releases, info)), "application/json", SHORT),
    ]
    if not upload(uploads, args.dry_run):
        print(json.dumps(info, indent=2))
        print(json.dumps(index, indent=2))
        return
    print("GroomLab for Blender %s is live: %s/%s" % (version, PUBLIC, latest))


if __name__ == "__main__":
    main()
