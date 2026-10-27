# SPDX-License-Identifier: Apache-2.0
"""Adds a new CPU node to GroomLab: its source file from the template and every place that lists it.

    python tools/dev/new_node.py Name [--category shape] [--gpu] [--dry-run]

Name is the node's type, CamelCase ("Frizz", "HairBend"): what graph JSON stores, never renamed once
grooms use it. It makes or edits:

  engine/core/src/operators/<name>_op.cpp   the operator, from tools/dev/new_node/example_op.cpp (a
                                            working node: params, evaluate, its NodeInfo)
  engine/core/include/groomlab/builtin_operators.inc
                                            its line, at the end of its category's section: op::kName,
                                            the register function's declaration and its call
  engine/core/CMakeLists.txt                the source file
  tests/engine/core/test_golden.cpp         a golden case (record its file, see the steps printed)
  web/content/docs/node-graph.mdx           its name in the Node categories table

--category: its add-node menu category (node_catalog.cpp's ids; default shape). --gpu: also print
what porting it to the GPU preview takes (a later change: a node lands on the CPU first).
--dry-run: change nothing, print what it would do and check every place it edits is where it
expects (a ctest runs this). docs/EXTENDING.md walks through the rest.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TEMPLATE = "tools/dev/new_node/example_op.cpp"
LIST = "engine/core/include/groomlab/builtin_operators.inc"
CATALOG = "engine/core/src/node_catalog.cpp"
CMAKE = "engine/core/CMakeLists.txt"
GOLDEN = "tests/engine/core/test_golden.cpp"
DOCS = "web/content/docs/node-graph.mdx"

# The operator's own category() (its role in a chain: presets look for "source" and "grow") for a
# menu category that is no role of its own.
ROLE = {"deform": "shape", "collide": "shape", "games": "look"}


class Plan(object):
    """The edits, made only once every one of them has found its place."""

    def __init__(self):
        self.files = {}  # path -> new text
        self.notes = []

    def read(self, path):
        if path in self.files:
            return self.files[path]
        with open(os.path.join(ROOT, path), encoding="utf-8", newline="") as f:
            return f.read()

    def write(self, path, text, note):
        self.files[path] = text
        self.notes.append((path, note))


def fail(message):
    sys.exit("new_node.py: " + message)


def snake(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def categories(plan):
    """[(id, title)] in menu order, from nodeCategories()."""
    text = plan.read(CATALOG)
    body = text[text.index("nodeCategories()"):]
    body = body[:body.index("};")]
    found = re.findall(r'\{"(\w+)", "([^"]+)"\}', body)
    if not found:
        fail("found no categories in %s's nodeCategories()" % CATALOG)
    return found


def make_source(plan, name, category):
    text = plan.read(TEMPLATE)
    # The template's opening comment (after the SPDX line, up to the first blank line) says what it is.
    head, sep, rest = text.partition("\n\n")
    if not head.startswith("// SPDX-License-Identifier: Apache-2.0") or not sep:
        fail("%s does not start with its SPDX line and a comment" % TEMPLATE)
    head = ("// SPDX-License-Identifier: Apache-2.0\n"
            "// %s: TODO what the node does, for the next reader. Made by tools/dev/new_node.py from its\n"
            "// template (%s), which moves each strand's points out from the scalp:\n"
            "// replace its params and evaluate() with your node's (docs/EXTENDING.md)." % (name, TEMPLATE))
    text = head + sep + rest
    role = ROLE.get(category, category)
    for old, new in (('std::string category() const override { return "shape"; }',
                      'std::string category() const override { return "%s"; }' % role),
                     ('{"shape", "Moves strands out from the scalp, more toward the tip.", "offset push lift"}',
                      '{"%s", "TODO: what it does, in a line (the param panel\'s top, the search tooltip).", "TODO search words"}'
                      % category)):
        if old not in text:
            fail("%s no longer holds %r: update new_node.py with it" % (TEMPLATE, old))
        text = text.replace(old, new)
    text = text.replace("Example", name)
    path = "engine/core/src/operators/%s_op.cpp" % snake(name)
    if os.path.exists(os.path.join(ROOT, path)):
        fail("%s exists already" % path)
    plan.write(path, text, "the operator, from the template (role %s, menu category %s)" % (role, category))
    return path


def add_to_list(plan, name, category):
    text = plan.read(LIST)
    if re.search(r"^GROOMLAB_OPERATOR(_ELSEWHERE)?\(%s\)" % name, text, re.M):
        fail("%s is listed in %s already" % (name, LIST))
    section = "// [%s]\n" % category
    start = text.find(section)
    if start < 0:
        fail("%s has no section %r" % (LIST, section.strip()))
    end = text.find("// [", start + len(section))
    if end < 0:
        fail("%s's section %r has no end (the next '// [' line)" % (LIST, section.strip()))
    line = "GROOMLAB_OPERATOR(%s)" % name
    line += " " * max(1, 41 - len(line)) + "// Curves -> Curves (TODO: what it takes and gives)\n"
    plan.write(LIST, text[:end] + line + text[end:], "its line at the end of the [%s] section" % category)


def add_to_cmake(plan, source):
    text = plan.read(CMAKE)
    lines = [m for m in re.finditer(r"^    src/operators/\w+\.cpp\n", text, re.M)]
    if not lines:
        fail("%s lists no src/operators/*.cpp" % CMAKE)
    at = lines[-1].end()
    entry = "    src/operators/%s\n" % os.path.basename(source)
    plan.write(CMAKE, text[:at] + entry + text[at:], "the source file, after the other operators'")


def add_golden_case(plan, name):
    text = plan.read(GOLDEN)
    start = text.find("std::vector<Case> cases() {\n    return {\n")
    if start < 0:
        fail("%s has no cases() list" % GOLDEN)
    end = text.find("\n    };\n}\n", start)
    if end < 0:
        fail("%s's cases() list has no end" % GOLDEN)
    case = ("\n        // %s (tools/dev/new_node.py): record tests/data/golden/%s.json with GROOMLAB_UPDATE_GOLDEN=1,\n"
            "        // then review it. Give the node the params that show what it does.\n"
            '        {"%s", "op", R"({"name": "op", "type": "%s"})", link("interp", "op", "strands")},'
            % (name, name, name, name))
    plan.write(GOLDEN, text[:end] + case + text[end:], "a golden case (Operators/Golden.MatchesStoredOutput/%s)" % name)


def add_to_docs(plan, name, title):
    text = plan.read(DOCS)
    m = re.search(r"^\| %s \| ([^|\n]*) \|" % re.escape(title), text, re.M)
    if not m:
        fail("%s's Node categories table has no row for %s" % (DOCS, title))
    nodes = m.group(1).rstrip()
    new = text[:m.start(1)] + nodes + ", " + name + text[m.end(1):]
    plan.write(DOCS, new, "its name in the Node categories table's %s row" % title)


GPU_STEPS = """
Porting it to the GPU preview (its own change, after the CPU node is in: docs/EXTENDING.md "Making it GPU"):
  1. engine/gpu/shaders/look.slang: [shader("compute")] {name}CS, matching the CPU's result.
  2. engine/gpu/include/groomlab/look/look_kernels.inc: GROOMLAB_KERNEL({name}) (the enum, both
     backends' tables and the CMake build follow).
  3. engine/gpu/include/groomlab/look/look_stages.inc: a GROOMLAB_STAGE({name}, "{lower}", ...) row with
     the params the kernel matches the CPU on (StageKind, its name and the drags follow).
  4. engine/gpu/src/look_chain.cpp: readLookParams' case (its constants, its fields), the inputs it
     uploads; engine/gpu/src/look_engine.cpp: kernelsOf's case and its dispatch.
  5. tests/engine/gpu/test_look_engine.cpp: a case comparing the GPU with the CPU (both backends).
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("name", help="the node's type, CamelCase (what graph JSON stores)")
    ap.add_argument("--category", default="shape", help="its add-node menu category (default shape)")
    ap.add_argument("--gpu", action="store_true", help="also print what porting it to the GPU preview takes")
    ap.add_argument("--dry-run", action="store_true", help="change nothing: print the plan and check its places")
    args = ap.parse_args()

    name = args.name
    if not re.match(r"^[A-Z][A-Za-z0-9]*$", name):
        fail("the name must be CamelCase letters and digits, starting with a capital: %r" % name)
    if name.startswith("Test"):
        fail("names starting with Test are the tests' own operators")
    plan = Plan()
    cats = categories(plan)
    titles = dict(cats)
    if args.category not in titles:
        fail("no category %r: one of %s" % (args.category, ", ".join(c for c, _ in cats)))

    source = make_source(plan, name, args.category)
    add_to_list(plan, name, args.category)
    add_to_cmake(plan, source)
    add_golden_case(plan, name)
    add_to_docs(plan, name, titles[args.category])

    for path, note in plan.notes:
        print("%s %s: %s" % ("would write" if args.dry_run else "wrote", path, note))
    if not args.dry_run:
        for path, text in plan.files.items():
            full = os.path.join(ROOT, path)
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(text)

    build = "powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2022"
    print("""
Next:
  1. Fill in {source}: what it does (the opening comment, the NodeInfo's description and search
     words), its params (ParamSpec: units, ranges, groups) and evaluate().
  2. Say what it takes and gives on its line in {list}.
  3. Build: {build}
  4. Record its golden file, then review it (the golden case's params should show what it does):
       set GROOMLAB_UPDATE_GOLDEN=1 (PowerShell: $env:GROOMLAB_UPDATE_GOLDEN=1)
       build/ninja-maya2022/tests/RelWithDebInfo/groomlab_tests --gtest_filter=Operators/Golden.MatchesStoredOutput/{name}
     then unset it and run the core tests: ctest -C RelWithDebInfo -L core (in build/ninja-maya2022).
     ParamUse.* fails for a param no state reads: read it, or allowlist it with who does
     (tests/engine/core/test_param_use.cpp).
  5. Docs: its row in {docs}'s Node categories table is in; say more where it helps (docs/reference/nodes.md,
     docs/USER_GUIDE.md) and in the release notes.""".format(source=source, list=LIST, build=build, name=name, docs=DOCS))
    if args.gpu:
        print(GPU_STEPS.format(name=name, lower=name[0].lower() + name[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
