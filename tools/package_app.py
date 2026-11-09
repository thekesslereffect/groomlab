# SPDX-License-Identifier: Apache-2.0
"""Package the standalone GroomLab app for a Windows beta: one zip that runs wherever it is unzipped.

On macOS (run on the Mac; main_mac below): the App component of build/macos-release (no TBB, macOS 13.3)
as GroomLab.app, Qt deployed into it by Qt's macdeployqt, every Mach-O checked to load nothing from
outside the bundle but the system, signed ad hoc (a Developer ID signature and notarization are for a
public release: docs/MACOS.md), zipped with ditto to dist/GroomLab-App-<version>-beta-macos-arm64.zip and
run once from a clean folder.

    python tools/package_app.py [--build build/ninja-app] [--config RelWithDebInfo] [--out dist]
                                [--stage-only] [--no-smoke] [--no-window]

1. Installs the build's App component (cmake --install --component App, hosts/app/CMakeLists.txt)
   into a clean staging folder: GroomLab.exe, Qt's DLLs and plugins (windeployqt), tbb.dll, the
   MSVC runtime app-local, the graph presets, the sample meshes (meshes/: CC BY 4.0), THIRD_PARTY_NOTICES.txt, licenses/ (Qt's LGPL
   notice and texts) and README_BETA.txt (hosts/app/package).
2. Checks it: every file the manifest below requires is there, nothing it forbids (symbols, test
   programs, Qt's debug DLLs, translations) is, and every DLL each binary imports is in the folder
   or in Windows. Writes licenses/QT_THIRD_PARTY.txt from Qt's software bill of materials (the
   third-party code inside the Qt files shipped; each Qt file checked against Qt's own checksum:
   unmodified Qt) and VERSION.txt.
3. Zips it to dist/GroomLab-App-<version>-beta-windows-x64.zip (one folder, GroomLab-App-<version>-beta).
4. The smoke run (unless --no-smoke): unzips the zip into a new temp folder outside the repo and
   runs GroomLab.exe from there with a PATH of Windows' folders alone, no Qt variables, its own
   settings folders: it opens the sample groom, saves it and renders a screenshot without a
   window (the offscreen platform), exports it as Alembic, then shows the window for a
   moment (the windows platform, Direct3D: the viewport and the floating UI), grabs it and quits
   (--no-window skips that step; no dialog ever opens). Every module the process loads is
   watched: one loaded from the build tree, the Qt install or the repo fails the run, and so does
   a GroomLab file loaded from anywhere but the unzipped folder.

--stage-only stops after 2 (ctest's app.package_manifest). tools/publish_app.py uploads the zip.
"""

import argparse
import ctypes
import filecmp
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGE = os.path.join(ROOT, "hosts", "app", "package")
INSTALL_DIR = "GroomLabApp"  # GROOMLAB_APP_DIR in hosts/app/CMakeLists.txt
SAMPLE = os.path.join(ROOT, "tests", "data", "strokes", "strokes.groom")

# The package's manifest: paths relative to the app folder.
REQUIRED = [
    "GroomLab.exe",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll",
    "platforms/qwindows.dll", "platforms/qoffscreen.dll",
    "styles/qmodernwindowsstyle.dll",
    "iconengines/qsvgicon.dll",  # the UI's icons are SVG
    "imageformats/qsvg.dll", "imageformats/qjpeg.dll",
    "tls/qschannelbackend.dll",  # HTTPS to the update feed (Windows' TLS)
    "tbb.dll",
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll",
    "LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.txt", "README_BETA.txt",
    "licenses/QT_NOTICE.txt", "licenses/LGPL-3.0.txt", "licenses/GPL-3.0.txt",
    # GroomLab > Add Sample Mesh (hosts/maya/meshes, CC BY 4.0)
    "meshes/nika_head.obj", "meshes/manny_head.obj", "meshes/wolf.obj",
]
FORBIDDEN = [
    r".*\.(pdb|ilk|lib|exp)$",
    r"translations/.*", r"generic/.*",  # no translations; no TUIO touch plugin
    r"(dxcompiler|dxil|d3dcompiler_47|opengl32sw)\.dll",  # Windows' or unused
    r"tls/qopensslbackend\.dll",  # no OpenSSL shipped: Windows' Schannel does TLS
    r".*_tests?\.exe", r"groomlab-helper.*", r"groomlab-cli.*",
]


def is_debug_qt(rel, lower):
    """Qt's debug build of a file: its release name with a "d" (Qt6Cored.dll, platforms/qwindowsd.dll)."""
    return rel.lower().endswith("d.dll") and rel.lower()[:-5] + ".dll" in lower


def cache_value(build, name):
    with open(os.path.join(build, "CMakeCache.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].strip()
    return None


# ---- PE imports (the static dependency check) ------------------------------------------------------

def pe_imports(path):
    """The DLL names a PE file imports: (normal, delay-loaded)."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return [], []
    sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    directories = optional + (112 if struct.unpack_from("<H", data, optional)[0] == 0x20B else 96)
    table = optional + optional_size
    spans = [struct.unpack_from("<IIII", data, table + 40 * i + 8) for i in range(sections)]

    def offset(rva):
        for size, address, raw_size, raw in spans:
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        return None

    def name_at(rva):
        o = offset(rva)
        return data[o:data.index(b"\0", o)].decode("ascii") if o is not None else None

    def walk(index, stride, name_field):
        rva = struct.unpack_from("<I", data, directories + 8 * index)[0]
        o = offset(rva) if rva else None
        names = []
        while o is not None:
            entry = data[o:o + stride]
            if len(entry) < stride or not any(entry):
                break
            name = name_at(struct.unpack_from("<I", entry, name_field)[0])
            if name:
                names.append(name)
            o += stride
        return names

    return walk(1, 20, 12), walk(13, 32, 4)


# DLLs a plugin's host has loaded before it loads the plugin, never shipped: the RenderMan procedural
# (procedurals/renderman) runs inside prman, which brings libprman.
HOST_PROVIDED = {"procedurals/renderman/": {"libprman.dll"}}


def check_imports(folder):
    """Every DLL a binary in `folder` imports is in the folder or in Windows (System32, an API set),
    or (HOST_PROVIDED) the renderer that loads it has it."""
    present = {n.lower() for n in os.listdir(folder)}
    system = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    missing = []
    for path in glob.glob(os.path.join(folder, "**", "*.dll"), recursive=True) + glob.glob(os.path.join(folder, "*.exe")):
        normal, _delayed = pe_imports(path)
        rel = os.path.relpath(path, folder).replace(os.sep, "/").lower()
        provided = set().union(*[names for prefix, names in HOST_PROVIDED.items() if rel.startswith(prefix)])
        for name in normal:
            low = name.lower()
            if low in present or low in provided or low.startswith(("api-ms-win-", "ext-ms-")) or \
                    os.path.isfile(os.path.join(system, name)):
                continue
            missing.append("%s needs %s" % (os.path.relpath(path, folder), name))
    return missing


# ---- The manifest and Qt's third-party notices ------------------------------------------------------

def files_of(folder):
    return sorted(os.path.relpath(os.path.join(d, n), folder).replace(os.sep, "/") for d, _, ns in os.walk(folder) for n in ns)


def check_manifest(folder):
    files = files_of(folder)
    lower = {f.lower() for f in files}
    problems = ["missing %s" % r for r in REQUIRED if r.lower() not in lower]
    problems += ["shipped but must not be: %s" % f for f in files
                 if any(re.fullmatch(p, f, re.IGNORECASE) for p in FORBIDDEN) or is_debug_qt(f, lower)]
    source_presets = files_of(os.path.join(ROOT, "hosts", "maya", "presets"))
    shipped_presets = [f[len("presets/"):] for f in files if f.startswith("presets/")]
    if not source_presets or shipped_presets != source_presets:
        problems.append("presets/ differs from hosts/maya/presets")
    source_meshes = [f for f in files_of(os.path.join(ROOT, "hosts", "maya", "meshes")) if f.lower().endswith(".obj")]
    shipped_meshes = [f[len("meshes/"):] for f in files if f.startswith("meshes/")]
    if not source_meshes or shipped_meshes != source_meshes:
        problems.append("meshes/ differs from the OBJs of hosts/maya/meshes")
    source_nodes = files_of(os.path.join(ROOT, "hosts", "maya", "nodes"))
    shipped_nodes = [f[len("nodes/"):] for f in files if f.startswith("nodes/")]
    if not source_nodes or shipped_nodes != source_nodes:
        problems.append("nodes/ differs from hosts/maya/nodes")
    source_alphas = files_of(os.path.join(ROOT, "hosts", "maya", "alphas"))
    shipped_alphas = [f[len("alphas/"):] for f in files if f.startswith("alphas/")]
    if not source_alphas or shipped_alphas != source_alphas:
        problems.append("alphas/ differs from hosts/maya/alphas")
    problems += check_imports(folder)
    return problems


def _component_name(package):
    m = re.search(r"^\s*Name: (.+)$", package.get("comment", ""), re.MULTILINE)
    return m.group(1).strip() if m else package["name"]


def qt_third_party(folder, qt_dir, qt_files=None, compare=True, beside="GroomLab.exe"):
    """licenses/QT_THIRD_PARTY.txt's text from Qt's SBOM: the third-party components inside the Qt
    files in `folder`, and the problems found (a Qt file the SBOM doesn't know, or one that is not
    the Qt install's own: a modified Qt). macOS passes its Qt files (the bundle's frameworks and
    plugins) and compare=False: macdeployqt rewrites their load commands, so no file is byte for byte
    Qt's."""
    docs = []
    for path in sorted(glob.glob(os.path.join(qt_dir, "sbom", "*.spdx.json"))):
        with open(path, encoding="utf-8") as f:
            docs.append(json.load(f))
    if not docs:
        return None, ["no Qt SBOM in %s/sbom" % qt_dir]
    packages, depends, file_owner, file_ids = {}, {}, {}, {}
    for d in docs:
        packages.update({p["SPDXID"]: p for p in d["packages"]})
        for f in d.get("files", []):
            file_ids.setdefault(os.path.basename(f["fileName"]).lower(), f["SPDXID"])
        for r in d["relationships"]:
            if r["relationshipType"] == "DEPENDS_ON":
                depends.setdefault(r["spdxElementId"], []).append(r["relatedSpdxElement"])
            elif r["relationshipType"] == "CONTAINS" and "PackagedFile" in r["relatedSpdxElement"]:
                file_owner[r["relatedSpdxElement"]] = r["spdxElementId"]
    problems, found = [], {}
    # (Qt's: Qt6*.dll and its plugin folders; GroomLab's own RenderMan procedural folder is not Qt's.)
    if qt_files is None:
        qt_files = [f for f in files_of(folder) if f.lower().endswith(".dll") and not f.startswith("procedurals/") and
                    (os.path.basename(f).startswith("Qt6") or "/" in f)]
    for rel in qt_files:
        name = os.path.basename(rel).lower()
        if name not in file_ids:
            problems.append("%s is not in Qt's SBOM" % rel)
            continue
        file_id = file_ids[name]
        # Qt's own file, as installed (the SBOM's checksums are of the files before Qt signed them).
        original = os.path.join(qt_dir, "plugins", rel) if "/" in rel else os.path.join(qt_dir, "bin", rel)
        if compare and (not os.path.isfile(original) or not filecmp.cmp(original, os.path.join(folder, rel), shallow=False)):
            problems.append("%s differs from %s: not Qt's binary" % (rel, original))
        todo, seen = [file_owner.get(file_id)], set()
        while todo:
            pid = todo.pop()
            if not pid or pid in seen or pid not in packages:
                continue
            seen.add(pid)
            if "-3rdparty-" in pid:
                p = packages[pid]
                found.setdefault((_component_name(p), p.get("versionInfo", "")), p)
            todo += depends.get(pid, [])
    extracted = {e["licenseId"]: e.get("extractedText", "") for d in docs for e in d.get("hasExtractedLicensingInfos", [])}
    out = ["Third-party code inside the Qt libraries shipped with GroomLab",
           "",
           "The Qt files beside %s (licenses/QT_NOTICE.txt) contain the components below, each" % beside,
           "under its own license. Listed from Qt's software bill of materials (the sbom folder of the Qt",
           "6.8.3 binaries); the source of each is in Qt's source archive (QT_NOTICE.txt) under",
           "qtbase/src/3rdparty or qtsvg/src/3rdparty, with its license file.",
           ""]
    licenses = set()
    for (name, version), p in sorted(found.items(), key=lambda kv: kv[0][0].lower()):
        license = p.get("licenseConcluded") or p.get("licenseDeclared") or "NOASSERTION"
        licenses.update(t for t in re.findall(r"[A-Za-z0-9.+\-]+", license) if t not in ("AND", "OR", "WITH", "NOASSERTION"))
        out += ["=" * 78, "%s %s" % (name, version) if version and version != "unknown" else name, "=" * 78,
                "License: %s" % license]
        if p.get("downloadLocation") not in (None, "NOASSERTION", "NONE"):
            out.append("Source: %s" % p["downloadLocation"])
        out += ["", p.get("copyrightText", "").strip() or "(no copyright line recorded)", ""]
    out += ["", "#" * 78, "License texts", "#" * 78, ""]
    for license in sorted(licenses):
        text = extracted.get(license)
        spdx = os.path.join(PACKAGE, "spdx", license + ".txt")
        if text is None and os.path.isfile(spdx):
            with open(spdx, encoding="utf-8") as f:
                text = f.read()
        out += ["=" * 78, license, "=" * 78, ""]
        out.append(text.strip() if text else "The %s license: https://spdx.org/licenses/%s.html (and the component's "
                                              "license file in Qt's source archive)." % (license, license))
        out.append("")
    return "\n".join(out) + "\n", problems


# ---- The smoke run ----------------------------------------------------------------------------------

class _ModuleEntry(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_uint32), ("th32ModuleID", ctypes.c_uint32), ("th32ProcessID", ctypes.c_uint32),
                ("GlblcntUsage", ctypes.c_uint32), ("ProccntUsage", ctypes.c_uint32), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", ctypes.c_uint32), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


def _modules_of(pid):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(0x18, pid)  # TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32
    if not snap or snap == ctypes.c_void_p(-1).value:
        return set()
    paths = set()
    try:
        entry = _ModuleEntry()
        entry.dwSize = ctypes.sizeof(_ModuleEntry)
        ok = k32.Module32FirstW(ctypes.c_void_p(snap), ctypes.byref(entry))
        while ok:
            paths.add(entry.szExePath)
            ok = k32.Module32NextW(ctypes.c_void_p(snap), ctypes.byref(entry))
    finally:
        k32.CloseHandle(ctypes.c_void_p(snap))
    return paths


def run_watched(exe, args, env, cwd, timeout=180):
    """Runs the app, gathering every module it loads while it runs: (exit code, output, modules)."""
    proc = subprocess.Popen([exe] + args, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    modules, done = set(), threading.Event()

    def watch():
        while not done.is_set():
            modules.update(_modules_of(proc.pid))
            time.sleep(0.02)

    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        out += b"\n(killed: no exit after %d s: a dialog or a hang)" % timeout
        proc.returncode = -1
    done.set()
    watcher.join()
    return proc.returncode, out.decode("utf-8", "replace"), modules


def smoke(zip_path, build, qt_dir, window=True):
    """Runs the zipped app from a fresh folder outside the repo; returns the problems."""
    work = tempfile.mkdtemp(prefix="groomlab-app-smoke-")
    print("smoke run in %s" % work)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(os.path.join(work, "app"))
    app = glob.glob(os.path.join(work, "app", "*"))[0]
    exe = os.path.join(app, "GroomLab.exe")
    data, out = os.path.join(work, "data"), os.path.join(work, "out")
    os.makedirs(data)
    os.makedirs(out)
    groom = os.path.join(data, "sample.groom")
    shutil.copy2(SAMPLE, groom)

    windows = os.environ.get("SystemRoot", r"C:\Windows")
    env = {k: os.environ[k] for k in ("SystemRoot", "windir", "SystemDrive", "COMPUTERNAME", "USERNAME", "USERPROFILE",
                                      "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "OS", "PATHEXT", "ComSpec")
           if k in os.environ}
    env.update({
        "PATH": ";".join([os.path.join(windows, "System32"), windows, os.path.join(windows, "System32", "Wbem")]),
        "TEMP": work, "TMP": work,
        "APPDATA": os.path.join(work, "roaming"), "LOCALAPPDATA": os.path.join(work, "local"),  # its settings, not yours
        "GROOMLAB_SETTINGS_DIR": os.path.join(work, "roaming"),  # (QSettings: Windows' Roaming folder whatever APPDATA says)
        "GROOMLAB_NO_UPDATES": "1",
        "QT_FORCE_STDERR_LOGGING": "1", "QT_DEBUG_PLUGINS": "1",
    })
    for d in (env["APPDATA"], env["LOCALAPPDATA"]):
        os.makedirs(d)

    # (label, arguments, the files it writes, the platform plugin)
    runs = [
        ("open, save and render", [groom, "--save", os.path.join(out, "saved.groom"), "--screenshot",
                                   os.path.join(out, "render.png"), "--size", "640x360"],
         [os.path.join(out, "saved.groom"), os.path.join(out, "render.png")], "offscreen"),
    ]
    runs.append(("an Alembic export", [groom, "--export-alembic", os.path.join(out, "export.abc")],
                 [os.path.join(out, "export.abc")], "offscreen"))
    if window:
        # The viewport widget needs a real platform (QRhi): the window shows for a moment, grabs
        # itself after its first frame and closes (no dialog opens in a scripted run).
        runs.append(("the window, its UI and viewport", [groom, "--ui-screenshot", os.path.join(out, "ui.png"), "--size", "1280x720"],
                     [os.path.join(out, "ui.png")], "windows"))
    problems, loaded = [], set()
    app_dir = os.path.normcase(os.path.abspath(app))
    banned = [os.path.normcase(os.path.abspath(p)) for p in (ROOT, build, qt_dir) if p]
    shipped = {n.lower() for n in files_of(app)} | {os.path.basename(n).lower() for n in files_of(app)}
    for label, args, outputs, platform in runs:
        start = time.time()
        code, log, modules = run_watched(exe, args, dict(env, QT_QPA_PLATFORM=platform), work)
        loaded |= modules
        missing = [o for o in outputs if not os.path.isfile(o) or os.path.getsize(o) == 0]
        status = "ok" if code == 0 and not missing else "FAILED (exit %s%s)" % (code, ", no " + ", ".join(missing) if missing else "")
        print("  %s: %s in %.1f s, %d modules" % (label, status, time.time() - start, len(modules)))
        if status != "ok":
            problems.append("%s failed:\n%s" % (label, log[-3000:]))
        for line in log.splitlines():  # QT_DEBUG_PLUGINS: what Qt loaded, from where
            m = re.search(r'"([^"]+\.dll)" loaded library', line, re.IGNORECASE)
            if m and not os.path.normcase(os.path.abspath(m.group(1))).startswith(app_dir):
                problems.append("Qt loaded %s from outside the app folder" % m.group(1))
    foreign = []
    for path in sorted(loaded):
        p = os.path.normcase(os.path.abspath(path))
        if p.startswith(app_dir):
            continue
        if any(p.startswith(b) for b in banned) or os.path.basename(p) in shipped:
            problems.append("loaded %s, not the app folder's" % path)
        elif not p.startswith(os.path.normcase(windows)):
            foreign.append(path)
    ours = sorted(os.path.relpath(p, app) for p in loaded if os.path.normcase(os.path.abspath(p)).startswith(app_dir))
    print("  loaded from the app folder: %s" % ", ".join(ours))
    if foreign:
        print("  loaded from elsewhere (not GroomLab's: drivers, hooks): %s" % ", ".join(foreign))
    if not problems:
        shutil.rmtree(work, ignore_errors=True)
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=os.path.join(ROOT, "build", "ninja-app"))
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--cmake", default="cmake", help="the cmake executable (default: the one on PATH)")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--stage-only", action="store_true", help="install and check the folder; no zip")
    parser.add_argument("--no-smoke", action="store_true", help="skip the smoke run from a clean folder")
    parser.add_argument("--no-window", action="store_true", help="smoke run without the step that shows the window")
    args = parser.parse_args()
    if sys.platform == "darwin":
        return main_mac(args)
    build = os.path.abspath(args.build)
    if not os.path.isfile(os.path.join(build, "CMakeCache.txt")):
        sys.exit("no build in %s: configure and build the ninja-app preset first" % build)
    version = cache_value(build, "CMAKE_PROJECT_VERSION")
    qt_dir = cache_value(build, "GROOMLAB_QT_DIR") or os.path.normpath(os.path.join(cache_value(build, "Qt6_DIR") or ".", "..", "..", ".."))

    stage = os.path.join(args.out, "app_stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    subprocess.check_call([args.cmake, "--install", build, "--config", args.config, "--component", "App", "--prefix", stage],
                          stdout=subprocess.DEVNULL)
    folder = os.path.join(stage, INSTALL_DIR)
    if not os.path.isfile(os.path.join(folder, "GroomLab.exe")):
        sys.exit("no GroomLab.exe in the install: build %s (%s) with GROOMLAB_BUILD_APP first" % (build, args.config))
    problems = check_manifest(folder)
    notices, qt_problems = qt_third_party(folder, qt_dir)
    problems += qt_problems
    if problems:
        sys.exit("the app package is wrong:\n  " + "\n  ".join(problems))
    with open(os.path.join(folder, "licenses", "QT_THIRD_PARTY.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(notices)
    files = files_of(folder)
    size = sum(os.path.getsize(os.path.join(folder, f)) for f in files)
    print("staged %d files (%.1f MB): the manifest and the imports check out, the Qt files are Qt's own" % (len(files), size / 1e6))
    if args.stage_only:
        return

    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(folder, "VERSION.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("GroomLab app %s beta, Windows x64 (%s, %s)\n" % (version, commit or "unknown commit", args.config))
    top = "GroomLab-App-%s-beta" % version
    zip_path = os.path.join(args.out, "%s-windows-x64.zip" % top)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in files_of(folder):
            z.write(os.path.join(folder, rel), "%s/%s" % (top, rel))
    shutil.rmtree(stage)
    print("wrote %s (%.1f MB, commit %s)" % (zip_path, os.path.getsize(zip_path) / 1e6, commit))

    if not args.no_smoke:
        problems = smoke(zip_path, build, qt_dir, not args.no_window)
        if problems:
            sys.exit("the smoke run failed:\n  " + "\n  ".join(problems))
        print("smoke run: the unzipped app runs on its own")


# ---- macOS ------------------------------------------------------------------------------------------

def _macho_files(app):
    """Every Mach-O file in the bundle (frameworks' binaries through their real paths, once)."""
    found = []
    for folder, _, files in os.walk(app):
        for name in files:
            path = os.path.join(folder, name)
            if os.path.islink(path):
                continue
            with open(path, "rb") as f:
                magic = f.read(4)
            if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
                found.append(path)
    return found


def _check_links(app):
    """Problems: a Mach-O in the bundle loading a library from outside it (anything but @rpath,
    @executable_path, @loader_path, /System and /usr/lib), or an rpath outside it."""
    problems = []
    for path in _macho_files(app):
        rel = os.path.relpath(path, app)
        out = subprocess.run(["otool", "-L", path], capture_output=True, text=True).stdout.splitlines()
        for line in out:
            if not line.startswith(("\t", " ")):  # the file's own header (one per architecture)
                continue
            dep = line.strip().split(" (")[0]
            if not dep.startswith(("@rpath/", "@executable_path/", "@loader_path/", "/System/", "/usr/lib/")):
                problems.append("%s loads %s" % (rel, dep))
        cmds = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout.splitlines()
        for i, line in enumerate(cmds):
            if line.strip() == "cmd LC_RPATH" and i + 2 < len(cmds):
                rpath = cmds[i + 2].split()[1]
                if not rpath.startswith(("@executable_path", "@loader_path")):
                    problems.append("%s has the rpath %s" % (rel, rpath))
    return problems


def _smoke_mac(zip_path, top):
    """Unzips the package into a new temp folder and runs the app from there without a window: the
    sample groom rendered to a PNG. Problems: no image, or a library loaded from outside the bundle
    (the Qt the build used, Homebrew)."""
    problems = []
    with tempfile.TemporaryDirectory(prefix="groomlab_app_smoke_") as tmp:
        subprocess.check_call(["ditto", "-x", "-k", zip_path, tmp])
        exe = os.path.join(tmp, top, "GroomLab.app", "Contents", "MacOS", "GroomLab")
        image = os.path.join(tmp, "smoke.png")
        env = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "DYLD_", "GROOMLAB_"))}
        env.update({"DYLD_PRINT_LIBRARIES": "1", "GROOMLAB_SETTINGS_DIR": os.path.join(tmp, "settings")})
        run = subprocess.run([exe, SAMPLE, "--screenshot", image, "--size", "320x240"], env=env, cwd=tmp,
                             capture_output=True, text=True, timeout=300)
        if run.returncode != 0 or not os.path.isfile(image):
            problems.append("no screenshot (exit %d): %s" % (run.returncode, run.stderr[-800:]))
        for line in run.stderr.splitlines():
            m = re.match(r"dyld\[\d+\]: <[^>]+> (.+)$", line)
            if m and not m.group(1).startswith(("/System/", "/usr/lib/", os.path.realpath(tmp), tmp)):
                problems.append("loaded from outside the bundle: %s" % m.group(1))
    return problems


def main_mac(args):
    build = os.path.abspath(args.build if args.build != os.path.join(ROOT, "build", "ninja-app")
                            else os.path.join(ROOT, "build", "macos-release"))
    if not os.path.isfile(os.path.join(build, "CMakeCache.txt")):
        sys.exit("no build in %s: configure and build the macos-release preset first" % build)
    version = cache_value(build, "CMAKE_PROJECT_VERSION")
    qt_dir = cache_value(build, "GROOMLAB_QT_DIR")
    stage = os.path.join(args.out, "app_stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    subprocess.check_call([args.cmake, "--install", build, "--config", args.config, "--component", "App", "--prefix", stage],
                          stdout=subprocess.DEVNULL)
    folder = os.path.join(stage, INSTALL_DIR)
    app = os.path.join(folder, "GroomLab.app")
    if not os.path.isfile(os.path.join(app, "Contents", "MacOS", "GroomLab")):
        sys.exit("no GroomLab.app in the install: build %s with GROOMLAB_BUILD_APP first" % build)
    # Qt into the bundle (Contents/Frameworks, Contents/PlugIns), then the offscreen platform too
    # (scripted runs and the smoke run below), which macdeployqt leaves out.
    subprocess.check_call([os.path.join(qt_dir, "bin", "macdeployqt"), app, "-always-overwrite"], stdout=subprocess.DEVNULL)
    offscreen = os.path.join(app, "Contents", "PlugIns", "platforms", "libqoffscreen.dylib")
    shutil.copy2(os.path.join(qt_dir, "plugins", "platforms", "libqoffscreen.dylib"), offscreen)
    lines = subprocess.run(["otool", "-l", offscreen], capture_output=True, text=True).stdout.splitlines()
    rpaths = {lines[i + 2].split()[1] for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"}  # (once per slice)
    for rpath in sorted(rpaths):
        subprocess.check_call(["install_name_tool", "-delete_rpath", rpath, offscreen], stderr=subprocess.DEVNULL)
    subprocess.check_call(["install_name_tool", "-add_rpath", "@loader_path/../../Frameworks", offscreen], stderr=subprocess.DEVNULL)
    # Apple silicon alone (docs/MACOS.md): Qt's universal binaries thinned to arm64.
    thin = app + ".arm64"
    subprocess.check_call(["ditto", "--arch", "arm64", app, thin])
    shutil.rmtree(app)
    os.rename(thin, app)
    problems = _check_links(app)
    contents = os.path.join("GroomLab.app", "Contents")
    qt_files = [os.path.relpath(p, folder) for p in _macho_files(app)
                if "/Frameworks/Qt" in p or (os.sep + "PlugIns" + os.sep) in p]
    notices, qt_problems = qt_third_party(folder, qt_dir, qt_files=qt_files, compare=False, beside="GroomLab.app")
    problems += qt_problems
    if problems:
        sys.exit("the app package is wrong:\n  " + "\n  ".join(problems))
    with open(os.path.join(folder, "licenses", "QT_THIRD_PARTY.txt"), "w", encoding="utf-8") as f:
        f.write(notices)
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(folder, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write("GroomLab app %s beta, macOS arm64 (%s, %s)\n" % (version, commit or "unknown commit", args.config))
    # Ad hoc (Apple silicon runs nothing unsigned; macdeployqt and install_name_tool broke the signatures).
    subprocess.check_call(["codesign", "--force", "--deep", "--sign", "-", app], stderr=subprocess.DEVNULL)
    subprocess.check_call(["codesign", "--verify", "--deep", "--strict", app])
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(folder) for f in fs
               if not os.path.islink(os.path.join(d, f)))
    print("staged GroomLab.app (%.1f MB): nothing loaded from outside the bundle, signed ad hoc, %d Qt files listed"
          % (size / 1e6, len(qt_files)))
    if args.stage_only:
        return
    top = "GroomLab-App-%s-beta" % version
    os.rename(folder, os.path.join(stage, top))
    zip_path = os.path.join(args.out, "%s-macos-arm64.zip" % top)
    if os.path.exists(zip_path):
        os.remove(zip_path)
    subprocess.check_call(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", os.path.join(stage, top), zip_path])
    shutil.rmtree(stage)
    print("wrote %s (%.1f MB, commit %s)" % (zip_path, os.path.getsize(zip_path) / 1e6, commit))
    if not args.no_smoke:
        problems = _smoke_mac(zip_path, top)
        if problems:
            sys.exit("the smoke run failed:\n  " + "\n  ".join(problems))
        print("smoke run: the unzipped app runs on its own")


if __name__ == "__main__":
    main()
