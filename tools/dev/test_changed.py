# SPDX-License-Identifier: Apache-2.0
"""Runs the tests of the areas a branch touched: the ctest labels of every file changed since it left
main (committed or not), the inner loop's test run. The full tiers (quick, gui, arnold) still run
once before merging (docs/DEVELOPING.md section 2).

    python tools/dev/test_changed.py [--build build/ninja-maya2022] [--base main] [--print] [-j N]

Without --build: build/ninja-app when the app's files changed (that tree builds the app's tests as well
as the rest), build/ninja-maya2022 otherwise.

Areas (ctest labels): core, io, gpu, python, maya, app, blender; gui, arnold and renderman run their sessions
when their files changed. A change to the core's headers, the build or the test data runs every quick test and
the Maya and Blender suites (no longer in quick). The timing tests (budget) never run here: ctest -L budget.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (path prefix, labels): the first match wins, so the specific ones come first.
RULES = [
    ("hosts/maya/scripts/groomlab/ui/", {"maya", "gui"}),
    ("hosts/maya/src/groom_draw_override", {"maya", "gui"}),
    ("hosts/maya/src/brush_tool", {"maya", "gui"}),
    ("hosts/maya/src/select_tool", {"maya", "gui"}),
    ("hosts/maya/src/reference_tool", {"maya", "gui"}),
    ("hosts/maya/src/tool_view", {"maya", "gui"}),
    ("hosts/maya/src/gpu_preview", {"maya", "gui", "gpu"}),
    ("hosts/maya/arnold/", {"maya", "arnold"}),
    ("hosts/maya/scripts/groomlab/renderman.py", {"maya", "renderman"}),  # its data string: prman reads it too
    ("hosts/arnold/", {"arnold"}),
    ("hosts/renderman/", {"renderman"}),
    ("tests/hosts/renderman/", {"renderman"}),
    ("engine/core/src/renderman_look", {"core", "renderman"}),
    ("tests/hosts/maya/gui_", {"gui"}),
    ("tests/hosts/maya/editor_cases", {"gui"}),
    ("tests/hosts/maya/test_arnold", {"arnold"}),
    ("hosts/maya/", {"maya"}),
    ("tests/hosts/maya/", {"maya"}),
    ("hosts/app/", {"app"}),
    ("tests/hosts/app/", {"app"}),
    ("hosts/blender/", {"blender"}),
    ("tests/hosts/blender/", {"blender"}),
    ("engine/core/include/", {"quick"}),  # every host builds on the core's headers
    ("engine/core/", {"core", "python", "blender"}),
    ("tests/engine/core/", {"core"}),
    ("engine/capi/", {"core"}),  # the C API's tests are core-labelled
    ("tests/engine/capi/", {"core"}),
    ("web/content/docs/c-api.mdx", {"core"}),  # capi.reference_docs checks it against the header
    ("tools/dev/capi_reference.py", {"core"}),
    ("web/content/docs/node-graph.mdx", {"core"}),  # Docs.NodeGraphListsEveryNode checks it against the registry
    ("tools/dev/new_node", {"core"}),  # its template is built into the core tests; tools.new_node_dry_run
    ("engine/io/", {"io", "python", "blender"}),
    ("tests/engine/io/", {"io"}),
    ("engine/gpu/", {"gpu"}),
    ("tests/engine/gpu/", {"gpu"}),
    ("engine/python/", {"python", "maya"}),  # the plugin's Python imports the module
    ("tests/engine/python/", {"python"}),
    ("engine/cli/", {"core", "python", "maya", "blender"}),  # core: cli.mcp_session
    ("tests/engine/cli/", {"core"}),
    ("tests/data/", {"quick"}),
    ("cmake/", {"quick"}),
    ("CMakeLists.txt", {"quick"}),
    ("tests/CMakeLists.txt", {"quick"}),
]
IGNORED = ("docs/", "web/", "README.md", "tools/bench/", ".github/")


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True).stdout


def changed_files(base):
    fork = git("merge-base", base, "HEAD").strip()
    names = set(git("diff", "--name-only", fork).split())  # committed since the fork, and the working tree
    names |= set(git("ls-files", "--others", "--exclude-standard").split())
    return sorted(names)


def labels_for(files):
    labels = set()
    for name in files:
        if name.startswith(IGNORED) and name not in ("web/content/docs/c-api.mdx", "web/content/docs/node-graph.mdx"):
            continue
        for prefix, found in RULES:
            if name == prefix or name.startswith(prefix) or (prefix.endswith(".txt") and name.endswith("/" + prefix)):
                labels |= found
                break
    return labels


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=None)
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--base", default="main")
    parser.add_argument("-j", "--jobs", type=int, default=12)
    parser.add_argument("--print", action="store_true", help="print the ctest commands, run nothing")
    args = parser.parse_args()

    files = changed_files(args.base)
    labels = labels_for(files)
    print("%d files changed since %s: labels %s" % (len(files), args.base, ", ".join(sorted(labels)) or "none"))
    if args.build is None:
        app_tree = os.path.join(ROOT, "build", "ninja-app")
        use_app = "app" in labels and os.path.isdir(app_tree)
        args.build = app_tree if use_app else os.path.join(ROOT, "build", "ninja-maya2022")
    runs = []
    windowless = sorted(labels - {"gui", "arnold", "renderman"})
    # The sessions, renders, timing and benchmarks have runs (or labels) of their own.
    excluded = ["-LE", "^(gui|arnold|renderman|vray|budget|bench|pictures)$"]
    if windowless:
        if "quick" in windowless:
            runs.append(["-L", "quick", "-j", str(args.jobs)] + excluded)
            windowless = ["maya", "blender"]  # (the host suites, out of quick)
        # Those areas' windowless tests.
        runs.append(["-L", "^(%s)$" % "|".join(windowless), "-j", str(args.jobs)] + excluded)
    for session in ("gui", "arnold", "renderman"):
        if session in labels:
            runs.append(["-L", session])
    code = 0
    for extra in runs:
        cmd = ["ctest", "--test-dir", args.build, "-C", args.config, "--output-on-failure"] + extra
        print(" ".join(cmd))
        if not args.print:
            code = subprocess.call(cmd) or code
    return code


if __name__ == "__main__":
    sys.exit(main())
