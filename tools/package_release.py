# SPDX-License-Identifier: Apache-2.0
"""Package GroomLab for Maya: one zip with the module for every Maya version built, and a
drag-and-drop installer.

    python tools/package_release.py [--maya 2022,2023,...] [--config RelWithDebInfo] [--symbols]

One module serves every Maya: the shared parts (scripts, presets, icons, themes, expressions, the
Arnold procedural and groomlab-cli, the docs) once, each Maya version's binaries (the plugin, the
groomlab_core Python module, the MtoA translator) in a folder of their own, and a GroomLab.mod whose
MAYAVERSION sections give each Maya its own: Maya 2022's at the module's root (plug-ins/, python/,
extensions/: the layout every earlier release had), every other version's in maya<year>/. One zip
for every Maya keeps one download, one install and one latest.json for Check for Updates; each
added Maya costs about 12 MB of it.

The builds are build/ninja-maya<year> (Maya 2022 the installed Maya, every other version its devkit:
docs/DEVELOPING.md, "Other Maya versions"). Without --maya the zip holds Maya 2022 and every other
version built. Maya 2022 must be there: it is the tested version and the only build with the Arnold
procedural and groomlab-cli. The other versions are beta (built, not yet tested in their Maya):
GroomLab/maya_versions.json says which, the installer and README_INSTALL.txt say so.

macOS (run on the Mac): the builds are build/macos-maya<year> (2024 to 2027, against each Maya install:
docs/MACOS.md), the plugin GroomLab.bundle in maya<year>/ for every version (no Maya 2022 there), and the
Arnold procedural and groomlab-cli from build/macos-release (no TBB, macOS 13.3). Every version is beta
(tested in mayapy, not yet in Maya's UI). The zip is dist/GroomLab-<version>-maya-macos-arm64.zip.

Installs each build's GroomLab component into a clean staging folder (cmake --install), joins the
.mod sections, adds tools/package/* (install.mel, install_groomlab.py, README_INSTALL.txt) and the
user guide, and zips it to dist/GroomLab-<version>-maya-win64.zip. Debug symbols (.pdb, ~150 MB per
version) are left out unless --symbols: without them crash logs still name the failing module, not
lines.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAC = sys.platform == "darwin"
if MAC:
    TESTED = ()               # tested in mayapy, not yet in Maya's UI: beta
    SUPPORTED = ("2024", "2025", "2026", "2027")
    PLUGIN = "GroomLab.bundle"
    RELEASE_BUILD = os.path.join(ROOT, "build", "macos-release")  # the procedural and groomlab-cli, without TBB
else:
    TESTED = ("2022",)        # Maya versions GroomLab is tested in; the others built are beta
    SUPPORTED = ("2022", "2023", "2024", "2025", "2026", "2027")
    PLUGIN = "GroomLab.mll"


def build_dir(year):
    return os.path.join(ROOT, "build", ("macos-maya%s" if MAC else "ninja-maya%s") % year)


def bin_dir(module, year):
    """A Maya version's binaries in the module: Maya 2022's at its root, the others' in maya<year>/."""
    return module if year == "2022" and not MAC else os.path.join(module, "maya%s" % year)


def mod_section(build):
    """The MAYAVERSION section of a build's GroomLab.mod (hosts/maya/module/GroomLab.mod.in)."""
    with open(os.path.join(build, "hosts", "maya", "GroomLab.mod")) as f:
        return [line.rstrip("\n") for line in f if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--maya", help="the Maya versions, comma separated (default: 2022 and every other built)")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--symbols", action="store_true", help="include the plugins' .pdb")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = parser.parse_args()

    if args.maya:
        years = [y.strip() for y in args.maya.split(",") if y.strip()]
    elif MAC:
        years = [y for y in SUPPORTED if os.path.isfile(os.path.join(build_dir(y), "CMakeCache.txt"))]
    else:
        years = [y for y in SUPPORTED if y == "2022" or os.path.isfile(os.path.join(build_dir(y), "CMakeCache.txt"))]
    unknown = [y for y in years if y not in SUPPORTED]
    if unknown:
        sys.exit("not a supported Maya version: %s (%s)" % (", ".join(unknown), ", ".join(SUPPORTED)))
    if not years:
        sys.exit("no Maya build to package (%s)" % build_dir("<year>"))
    if "2022" not in years and not MAC:
        sys.exit("Maya 2022 must be in the package: it is the tested version and holds the procedural and the CLI")
    years.sort()

    stage = os.path.join(args.out, "stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    module = os.path.join(stage, "GroomLab")
    sections = []
    # Maya 2022 last: its install's shared files (the same sources as every build's) are the ones kept.
    for year in sorted(years, key=lambda y: (y == "2022", y)):
        build = build_dir(year)
        subprocess.check_call(["cmake", "--install", build, "--config", args.config, "--component", "GroomLab",
                               "--prefix", stage], stdout=subprocess.DEVNULL)
        plugin = os.path.join(bin_dir(module, year), "plug-ins", PLUGIN)
        if not os.path.isfile(plugin):
            sys.exit("no %s in the install: build %s (%s) first" % (os.path.relpath(plugin, stage), build, args.config))
        section = mod_section(build)
        if not section or ("MAYAVERSION:%s " % year) not in section[0]:
            sys.exit("%s's GroomLab.mod is not Maya %s's: reconfigure it" % (build, year))
        sections.append((year, section))
        pdb = os.path.join(bin_dir(module, year), "plug-ins", "GroomLab.pdb")
        built_pdb = os.path.join(build, "hosts", "maya", args.config, "GroomLab.pdb")
        if args.symbols and os.path.isfile(built_pdb):
            shutil.copy2(built_pdb, pdb)
    sections.sort()
    if MAC:
        # The Arnold procedural and groomlab-cli from the release build (no TBB: one procedural for every
        # Maya's Arnold, built against the oldest), not a Maya build's (which carries that Maya's TBB).
        shutil.rmtree(os.path.join(module, "procedurals"), ignore_errors=True)
        shutil.rmtree(os.path.join(module, "bin"), ignore_errors=True)
        subprocess.check_call(["cmake", "--install", RELEASE_BUILD, "--config", args.config, "--component", "GroomLab",
                               "--prefix", stage], stdout=subprocess.DEVNULL)
        for need in ("procedurals/groomlab_procedural.dylib", "bin/groomlab-cli"):
            if not os.path.isfile(os.path.join(module, need)):
                sys.exit("no %s from %s: build it first (cmake --build --preset macos-release)" % (need, RELEASE_BUILD))
    # The RenderMan procedural comes with Maya 2022's build when it found RenderMan Pro Server
    # (GROOMLAB_RMANTREE); a machine without RenderMan packages without it.
    renderman = os.path.join(module, "procedurals", "renderman", "groomlab_renderman.dll")
    if MAC:
        print("note: no RenderMan procedural on macOS (it waits for RenderMan 28, the first for Apple silicon)")
    elif os.path.isfile(renderman):
        print("with the RenderMan procedural (procedurals/renderman)")
    else:
        print("note: no RenderMan procedural in the package (build %s found no RenderMan SDK: set "
              "GROOMLAB_RMANTREE or RMANTREE and reconfigure to include it)" % build_dir("2022"))
    with open(os.path.join(stage, "GroomLab.mod"), "w") as f:
        f.write("\n".join("\n".join(lines) for _, lines in sections) + "\n")

    for folder, dirs, files in os.walk(stage):
        for d in [d for d in dirs if d == "__pycache__"]:
            shutil.rmtree(os.path.join(folder, d))
        if not args.symbols:
            for name in files:
                if name.lower().endswith(".pdb"):
                    os.remove(os.path.join(folder, name))
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.txt"):  # groomlab_install_legal's
        if not os.path.isfile(os.path.join(module, name)):
            sys.exit("no %s in the module: the GroomLab component installs it (reconfigure the build)" % name)
    for name in os.listdir(os.path.join(ROOT, "tools", "package")):
        shutil.copy2(os.path.join(ROOT, "tools", "package", name), stage)
    docs = os.path.join(module, "docs")
    os.makedirs(docs, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, "docs", "USER_GUIDE.md"), docs)
    shutil.copy2(os.path.join(ROOT, "README.md"), docs)
    # The full reference (docs/reference, which USER_GUIDE.md and README.md link), for offline use.
    shutil.copytree(os.path.join(ROOT, "docs", "reference"), os.path.join(docs, "reference"), dirs_exist_ok=True)

    # Which Maya each build is for, and which are beta: the installer and Check for Updates read it.
    builds = {}
    for year in years:
        folder = bin_dir(module, year)
        builds[year] = {
            "status": "tested" if year in TESTED else "beta",
            "plugin": os.path.relpath(os.path.join(folder, "plug-ins", PLUGIN), module).replace("\\", "/"),
            "python": os.path.relpath(os.path.join(folder, "python"), module).replace("\\", "/"),
            "arnold": os.path.isdir(os.path.join(folder, "extensions")),
            "vray": os.path.isdir(os.path.join(folder, "vray")),
        }
        if not builds[year]["vray"]:
            print("note: no V-Ray for Maya translator for Maya %s (its build found no V-Ray for Maya %s: install it "
                  "on this machine, or set VRAY_FOR_MAYA_LOCATION, and reconfigure to include it)" % (year, year))
    with open(os.path.join(module, "maya_versions.json"), "w") as f:
        json.dump(builds, f, indent=2, sort_keys=True)
        f.write("\n")

    with open(os.path.join(stage, "GroomLab.mod")) as f:
        version = next(l.split()[3] for l in f if l.startswith("+"))
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(module, "VERSION.txt"), "w") as f:
        f.write("GroomLab %s (%s, %s)\n" % (version, commit or "unknown commit", args.config))

    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, "GroomLab-%s-maya-%s.zip" % (version, "macos-arm64" if MAC else "win64"))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for folder, _, files in os.walk(stage):
            for name in sorted(files):
                full = os.path.join(folder, name)
                z.write(full, os.path.join("GroomLab-%s" % version, os.path.relpath(full, stage)))
    shutil.rmtree(stage)
    size = os.path.getsize(zip_path) / 1e6
    beta = [y for y in years if y not in TESTED]
    print("wrote %s (%.1f MB, commit %s): Maya %s%s" % (zip_path, size, commit, ", ".join(years),
                                                       " (beta: %s)" % ", ".join(beta) if beta else ""))


if __name__ == "__main__":
    main()
