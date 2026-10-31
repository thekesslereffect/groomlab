# SPDX-License-Identifier: Apache-2.0
"""engine/ui/*.json, the UI structure every host reads (docs/UI_DEFINITIONS.md): each file valid
against engine/ui/ui.schema.json, every id a container lists defined, every reference found, every
action used somewhere. With the groomlab_core module importable, also against the core: shortcut ids
(shortcuts.h), the marking menu's tools (brush_catalog.h) and the built-in tube shapes. properties.json
against Maya's Attribute Editor (AEgroomLabShapeTemplate.mel: its sections, controls, custom blocks
and separators in order, its annotations as the tips, the attributes' nice names as the labels; the
Hair Color rows as ae.py's COLOR_ROWS) and Blender's properties (props.py). Maya's and
Blender's loaders (definitions.py) are held to the cases in resolve_cases.json, as the app's is by
its own test (AppUi.ResolveMatchesTheSharedCases).

    python tests/engine/ui/check_ui_definitions.py [engine/ui folder]

Exit code 0 when all of it passed (ctest ui.definitions). Needs no package: the schema subset the
files use is checked here.
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
FILES = ("actions", "menus", "palette", "lists", "choices", "marking_menu", "graph_ui", "properties")

errors = []


def fail(message):
    if message not in errors:  # a list referenced from two places: said once
        errors.append(message)


# -- The schema subset (draft 2020-12 keywords the schema uses) -------------------------------------

_TYPES = {"string": str, "boolean": bool, "array": list, "object": dict, "null": type(None)}


def _is_type(value, name):
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, _TYPES[name])


def validate(value, schema, root, where):
    """Problems of `value` against `schema` (a list of "where: what")."""
    problems = []
    if "$ref" in schema:
        node = root
        for part in schema["$ref"].lstrip("#/").split("/"):
            node = node[part]
        return validate(value, node, root, where)
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(value, t) for t in types):
            return ["%s: %r is not %s" % (where, value, "/".join(types))]
    if "const" in schema and value != schema["const"]:
        problems.append("%s: %r is not %r" % (where, value, schema["const"]))
    if "enum" in schema and value not in schema["enum"]:
        problems.append("%s: %r is not one of %r" % (where, value, schema["enum"]))
    if isinstance(value, str):
        if "pattern" in schema and not re.search(schema["pattern"], value):
            problems.append("%s: %r does not match %s" % (where, value, schema["pattern"]))
        if len(value) < schema.get("minLength", 0):
            problems.append("%s: empty" % where)
    if _is_type(value, "number"):
        if "minimum" in schema and value < schema["minimum"]:
            problems.append("%s: %r < %r" % (where, value, schema["minimum"]))
        if "maximum" in schema and value > schema["maximum"]:
            problems.append("%s: %r > %r" % (where, value, schema["maximum"]))
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", len(value)):
            problems.append("%s: %d items" % (where, len(value)))
        if "items" in schema:
            for i, item in enumerate(value):
                problems += validate(item, schema["items"], root, "%s[%d]" % (where, i))
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                problems.append("%s: no %r" % (where, key))
        properties = schema.get("properties", {})
        for key, item in value.items():
            if "propertyNames" in schema:
                problems += validate(key, schema["propertyNames"], root, "%s (key %r)" % (where, key))
            if key in properties:
                problems += validate(item, properties[key], root, "%s/%s" % (where, key))
            elif schema.get("additionalProperties") is False:
                problems.append("%s: unknown key %r" % (where, key))
            elif isinstance(schema.get("additionalProperties"), dict):
                problems += validate(item, schema["additionalProperties"], root, "%s/%s" % (where, key))
    for keyword in ("anyOf", "oneOf"):
        if keyword in schema:
            results = [validate(value, s, root, where) for s in schema[keyword]]
            passing = [r for r in results if not r]
            if not passing:
                # The closest choice's problems (an entry of the wrong shape: what is wrong with it),
                # past the choices of another type altogether.
                shaped = [r for r in results if not (r[0].startswith(where + ":") and " is not " in r[0])]
                problems += min(shaped or results, key=len)
            elif keyword == "oneOf" and len(passing) > 1:
                problems.append("%s: matches %d of the oneOf choices" % (where, len(passing)))
    return problems


# -- The files ------------------------------------------------------------------------------------

def load(folder):
    with io.open(os.path.join(folder, "ui.schema.json"), encoding="utf-8") as f:
        schema = json.load(f)
    merged = {}
    for name in FILES:
        path = os.path.join(folder, name + ".json")
        with io.open(path, encoding="utf-8") as f:
            data = json.load(f)
        for problem in validate(data, schema, schema, name + ".json"):
            fail(problem)
        for key, value in data.items():
            if key in ("$schema", "version"):
                continue
            if key in merged:
                fail("%s.json: %r is in another file too" % (name, key))
            merged[key] = value
    extra = sorted(f for f in os.listdir(folder) if f.endswith(".json") and f[:-5] not in FILES + ("ui.schema",))
    if extra:
        fail("files no host reads (add them to FILES in every host's loader): %r" % extra)
    return merged


def get(doc, path):
    node = doc
    for part in path.split("/"):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def walk(doc, entries, where, used):
    """Every entry of a container: ids defined, references found; used ids collected."""
    actions = doc["actions"]
    for i, entry in enumerate(entries):
        here = "%s[%d]" % (where, i)
        if entry == "-":
            continue
        if isinstance(entry, str):
            entry = {"id": entry}
        if "include" in entry or "ref" in entry:
            path = entry.get("include") or entry.get("ref")
            target = get(doc, path)
            if target is None:
                fail("%s: no %s" % (here, path))
            elif "items" not in entry:
                walk(doc, target["items"] if isinstance(target, dict) else target, path, used)
        if "items" in entry:
            walk(doc, entry["items"], here, used)
        if "generator" in entry:
            if entry.get("action"):
                used.add(entry["action"])
                if entry["action"] not in actions:
                    fail("%s: generator action %r is not in actions.json" % (here, entry["action"]))
            if entry["generator"] == "choices":
                options = doc["choices"].get(entry.get("choices", ""))
                if options is None:
                    fail("%s: no choices %r" % (here, entry.get("choices")))
                elif entry.get("labelField") and any(entry["labelField"] not in c for c in options["items"]):
                    fail("%s: an option of %s has no %r" % (here, entry["choices"], entry["labelField"]))
        elif "id" in entry and "submenu" not in entry and "ref" not in entry:
            used.add(entry["id"])
            if entry["id"] not in actions:
                fail("%s: %r is not in actions.json" % (here, entry["id"]))


def containers(doc):
    """(where, entries) of every container in the files."""
    out = []
    for name, value in doc["menus"].items():
        out.append(("menus/" + name, value["items"] if isinstance(value, dict) else value))
    palette = doc["palette"]
    out.append(("palette/output", palette["output"]))
    out.append(("palette/empty", palette.get("empty", {}).get("items", [])))
    for name, header in palette.get("header", {}).items():
        out.append(("palette/header/" + name, header.get("items", [])))
    for name, lists in doc["lists"].items():
        for key, value in lists.items():
            if isinstance(value, (list, dict)):
                out.append(("lists/%s/%s" % (name, key), value["items"] if isinstance(value, dict) else value))
    for key, value in doc["graph"].items():
        if key.endswith("Menu") or key in ("presetItem", "nodeActions"):
            out.append(("graph/" + key, value["items"] if isinstance(value, dict) else value))
    out.append(("markingMenu/tail", [e for e in doc["markingMenu"]["tail"] if isinstance(e, str)]))
    return out


def check(doc):
    used = set()
    for where, entries in containers(doc):
        walk(doc, entries, where, used)
    for aid, action in sorted(doc["actions"].items()):
        if aid not in used:
            fail("actions.json: %r is listed nowhere" % aid)
        if "{" in action.get("label", "") and not re.search(r"\{\w+\}", action["label"]):
            fail("actions.json: %r has a stray brace in its label" % aid)
    for name, options in doc["choices"].items():
        values = [c["value"] for c in options["items"]]
        if len(set(values)) != len(values):
            fail("choices.json: %s has a value twice" % name)
        if "default" in options and options["default"] not in values:
            fail("choices.json: %s's default %r is not one of its values" % (name, options["default"]))
    ids = [t["id"] for t in doc["palette"]["tabs"]]
    aliases = [a for t in doc["palette"]["tabs"] for a in t.get("aliases", [])]
    if len(set(ids + aliases)) != len(ids + aliases):
        fail("palette.json: a tab id or alias twice")
    for section in doc["palette"]["sections"]:
        for tab in section.get("tabs", []):
            if tab not in ids:
                fail("palette.json: section %s shows on tab %r, which is not a tab" % (section["id"], tab))
        if len(set(section.get("tools", []))) != len(section.get("tools", [])):
            fail("palette.json: section %s lists a tool twice" % section["id"])
    # The node actions: each for node types ("type.<operator>" in its needs), as the hosts pin them.
    for i, entry in enumerate(doc["graph"].get("nodeActions", {}).get("items", [])):
        if entry != "-" and not any(c.startswith("type.") for c in (entry.get("needs", []) if isinstance(entry, dict) else [])):
            fail("graph_ui.json: nodeActions[%d] names no node type (needs \"type.<operator>\")" % i)
    radial = [r["position"] for r in doc["markingMenu"]["radial"]]
    if len(set(radial)) != len(radial):
        fail("marking_menu.json: a radial position twice")


def check_core(doc):
    """Against the core, when its Python module is importable (the build's, on PYTHONPATH)."""
    try:
        import groomlab_core
    except ImportError:
        print("groomlab_core not importable: the core cross-checks are skipped")
        return
    rows = set()
    for host in ("maya", "blender", "app"):
        rows.update(r["id"] for r in groomlab_core.shortcuts(host))
    for aid, action in doc["actions"].items():
        if action.get("shortcut") and action["shortcut"] not in rows:
            fail("actions.json: %r's shortcut %r is not in the core's shortcuts.h" % (aid, action["shortcut"]))
        if "shortcutPart" in action:
            keys = [r["keys"] for h in ("maya", "blender", "app") for r in groomlab_core.shortcuts(h)
                    if r["id"] == action["shortcut"]]
            if keys and action["shortcutPart"] >= len(keys[0].split(" / ")):
                fail("actions.json: %r's shortcutPart is past %r" % (aid, keys[0]))
    tools = set(t["id"] for t in groomlab_core.brush_catalog())
    menu = doc["markingMenu"]
    for entry in menu["radial"] + [e for e in menu["tail"] if isinstance(e, dict)]:
        if entry["tool"] not in tools:
            fail("marking_menu.json: no tool %r in the brush catalog" % entry["tool"])
    for section in doc["palette"]["sections"]:
        for name in section.get("tools", []):
            if name not in tools:
                fail("palette.json: section %s shows with %r, not in the brush catalog" % (section["id"], name))
    if hasattr(groomlab_core, "operator_specs_json"):
        types = set(o["type"] for o in json.loads(groomlab_core.operator_specs_json()))
        for entry in doc["graph"].get("nodeActions", {}).get("items", []):
            for c in (entry.get("needs", []) if isinstance(entry, dict) else []):
                if c.startswith("type.") and c[5:] not in types:
                    fail("graph_ui.json: nodeActions names %r, not an operator type" % c[5:])
    for name in menu["overflow"].get("exclude", []):
        if name not in tools:
            fail("marking_menu.json: overflow excludes %r, not in the brush catalog" % name)
    if hasattr(groomlab_core, "tube_shapes"):
        builtin = [s if isinstance(s, str) else s.get("name") for s in groomlab_core.tube_shapes()]
        shapes = [c["value"] for c in doc["choices"]["tubeShape"]["items"]]
        if builtin and shapes != list(builtin):
            fail("choices.json: tubeShape %r is not the core's built-in shapes %r" % (shapes, builtin))


# -- properties.json against Maya's Attribute Editor -----------------------------------------------

AE_TEMPLATE = os.path.join(ROOT, "hosts", "maya", "scripts", "AEgroomLabShapeTemplate.mel")
AE_PY = os.path.join(ROOT, "hosts", "maya", "scripts", "groomlab", "ui", "ae.py")
SHAPE_CPP = os.path.join(ROOT, "hosts", "maya", "src", "groom_shape.cpp")
BLENDER_PROPS = os.path.join(ROOT, "hosts", "blender", "addon", "groomlab", "props.py")


def _strings(text):
    """The MEL string literals in `text`, joined (a "..." + "..." annotation)."""
    return "".join(m.replace('\\"', '"') for m in re.findall(r'"((?:[^"\\]|\\.)*)"', text))


def ae_layout(path=AE_TEMPLATE):
    """Maya's Attribute Editor for the groom as tokens, in order: ("begin", label, expanded or None),
    ("control", attr, annotation or None), ("custom", proc, attr), ("-",), ("end",)."""
    with io.open(path, encoding="utf-8") as f:
        text = "\n".join(line.split("//")[0] for line in f.read().splitlines())
    body = text[text.index("global proc AEgroomLabShapeTemplate"):]
    # The statements: split at the semicolons outside strings (an annotation may hold one).
    statements, current, quoted = [], [], False
    for i, c in enumerate(body):
        if c == '"' and body[i - 1] != "\\":
            quoted = not quoted
        if c == ";" and not quoted:
            statements.append("".join(current))
            current = []
        else:
            current.append(c)
    tokens = []
    for statement in statements:
        st = " ".join(statement.split())
        if "editorTemplate" not in st or "-suppress" in st:
            continue
        m = re.search(r'-beginLayout "([^"]+)"(?: -collapse (\S+))?', st)
        if m:
            collapse = m.group(2)
            tokens.append(("begin", m.group(1), None if collapse not in ("0", "1") else collapse == "0"))
        elif "-endLayout" in st:
            tokens.append(("end",))
        elif "-addSeparator" in st:
            tokens.append(("-",))
        elif "-callCustom" in st:
            m = re.search(r'-callCustom "(AE\w+?)New" "AE\w+?Replace" "(\w+)"', st)
            tokens.append(("custom", m.group(1), m.group(2)))
        elif "-addControl" in st:
            m = re.search(r'(?:-annotation (.*?) )?-addControl "(\w+)"', st)
            tokens.append(("control", m.group(2), _strings(m.group(1)) if m.group(1) else None))
    return tokens


def for_host(entry, host):
    return "hosts" not in entry or host in entry["hosts"]


def host_text(entry, key, host):
    return entry.get(key + "s", {}).get(host, entry.get(key, ""))


def property_tokens(sections, host="maya"):
    """properties.json's sections for `host` as ae_layout's tokens (annotations: the tips)."""
    out = []
    for section in sections:
        if not for_host(section, host):
            continue
        out.append(("begin", host_text(section, "label", host), section.get("expanded", False)))
        for item in section["items"]:
            if item == "-":
                out.append(("-",))
            elif for_host(item, host):
                if "custom" in item:
                    out.append(("custom", item["custom"], item.get("maya")))
                else:
                    out.append(("control", item.get("maya"), host_text(item, "tip", host)))
        out += property_tokens(section.get("sections", []), host)
        out.append(("end",))
    return out


def nice_name(attr, overrides):
    """Maya's nice name of an attribute: camel case as words, the first capitalized."""
    if attr in overrides:
        return overrides[attr]
    words = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", attr)
    return words[0].upper() + words[1:]


def check_properties(doc):
    """properties.json as Maya's Attribute Editor lays the groom out (Maya's look is the reference:
    a change goes into both), Hair Color's rows as ae.py's, Blender's properties as props.py has them."""
    props = doc["properties"]
    ids = set()

    def walk_ids(sections):
        for section in sections:
            for item in section["items"]:
                if item == "-":
                    continue
                if item["id"] in ids:
                    fail("properties.json: item %r twice" % item["id"])
                ids.add(item["id"])
                parts = [p["id"] for p in item.get("parts", [])]
                if len(set(parts)) != len(parts):
                    fail("properties.json: %s has a part twice" % item["id"])
                if for_host(item, "maya") and not item.get("maya"):
                    fail("properties.json: %r shows in Maya without a Maya attribute (maya)" % item["id"])
            walk_ids(section.get("sections", []))
    walk_ids(props["sections"])

    try:
        ae = ae_layout()
    except (IOError, OSError, ValueError, AttributeError) as e:
        fail("properties.json: cannot read Maya's AE template: %s" % e)
        return
    ours = property_tokens(props["sections"])
    for i in range(max(len(ae), len(ours))):
        a = ae[i] if i < len(ae) else None
        o = ours[i] if i < len(ours) else None
        same = a is not None and o is not None and a[0] == o[0] and a[1:2] == o[1:2]
        if not same:
            fail("properties.json: Maya's AE has %r at %d where properties.json has %r (the order, sections and "
                 "attributes must be Maya's)" % (a, i, o))
            break
        if a[0] == "begin" and a[2] is not None and a[2] != o[2]:
            fail("properties.json: section %r is %s in Maya's AE" % (a[1], "expanded" if a[2] else "folded"))
        if a[0] == "custom" and a[2] != o[2]:
            fail("properties.json: custom %r is on %r in Maya's AE, not %r" % (a[1], a[2], o[2]))
        if a[0] == "control" and a[2] is not None and a[2] != o[2]:
            fail("properties.json: %r's tip is not Maya's annotation %r" % (a[1], a[2]))

    # Labels: the attributes' nice names as Maya shows them.
    overrides = {}
    try:
        with io.open(SHAPE_CPP, encoding="utf-8") as f:
            cpp = f.read()
        for m in re.finditer(r'setNiceNameOverride\("([^"]+)"\)', cpp):
            made = re.findall(r'\.create\w*\("(\w+)"', cpp[:m.start()])
            if made:
                overrides[made[-1]] = m.group(1)
    except (IOError, OSError):
        pass

    def walk_labels(sections):
        for section in sections:
            for item in section["items"]:
                if item == "-" or not for_host(item, "maya") or "custom" in item:
                    continue
                want = nice_name(item["maya"], overrides)
                if host_text(item, "label", "maya") != want:
                    fail("properties.json: %r's label %r is not Maya's %r" % (item["id"], item["label"], want))
            walk_labels(section.get("sections", []))
    walk_labels(props["sections"])

    # The Hair Color rows: ae.py's COLOR_ROWS (param, label, kind, tip) in order, each shown in the
    # mode _COLOR_ONLY says.
    try:
        import ast
        with io.open(AE_PY, encoding="utf-8") as f:
            tree = ast.parse(f.read())

        def literal(name):
            return next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                        and any(getattr(t, "id", "") == name for t in n.targets))
        rows, only = literal("COLOR_ROWS"), literal("_COLOR_ONLY")
    except (IOError, OSError, StopIteration, ValueError, SyntaxError) as e:
        fail("properties.json: cannot read ae.py's COLOR_ROWS: %s" % e)
        rows, only = [], {}
    color = [i for s in props["sections"] for i in s["items"] if i != "-" and i.get("custom") == "AEgroomLabColor"]
    parts = color[0].get("parts", []) if color else []
    if rows and [(p["id"], p["label"], p.get("kind"), p.get("tip", "")) for p in parts] != [tuple(r) for r in rows]:
        fail("properties.json: the Hair Color parts are not ae.py's COLOR_ROWS (param, label, kind, tip, in order)")
    for p in parts:
        if only.get(p["id"]) != p.get("only"):
            fail("properties.json: Hair Color's %r shows in mode %r in ae.py, not %r" % (p["id"], only.get(p["id"]), p.get("only")))

    # Blender's properties exist.
    try:
        with io.open(BLENDER_PROPS, encoding="utf-8") as f:
            blender = set(re.findall(r"^\s+(\w+): \w+Property\(", f.read(), re.M))
    except (IOError, OSError):
        blender = None

    def walk_blender(sections):
        for section in sections:
            for item in section["items"]:
                if item != "-" and item.get("blender") and blender is not None and item["blender"] not in blender:
                    fail("properties.json: %r's Blender property %r is not in props.py" % (item["id"], item["blender"]))
            walk_blender(section.get("sections", []))
    walk_blender(props["sections"])

# -- The hosts' loaders against the shared cases --------------------------------------------------

CASES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "resolve_cases.json")
LOADERS = (("maya", os.path.join(ROOT, "hosts", "maya", "scripts", "groomlab", "ui", "definitions.py")),
           ("blender", os.path.join(ROOT, "hosts", "blender", "addon", "groomlab", "definitions.py")))


def project(items):
    """Resolved items as resolve_cases.json writes them (the app's test projects its own the same way)."""
    out = []
    for item in items:
        if item["kind"] == "divider":
            out.append("-")
        elif item["kind"] == "generator":
            out.append({"generator": item["name"]})
        elif item["kind"] == "submenu":
            out.append({"submenu": item["label"], "id": item["id"], "tip": item["tip"], "items": project(item["items"])})
        else:
            p = {"id": item["id"], "label": item["label"], "tip": item["tip"]}
            for key in ("value", "shortcut", "shortcutPart", "icon"):
                if item.get(key) is not None:
                    p[key] = item[key]
            if item["checkable"]:
                p["checkable"] = True
            if not item["enabled"]:
                p["enabled"] = False
            out.append(p)
    return out


def check_loaders():
    """Maya's and Blender's definitions.py resolve resolve_cases.json's cases as it says, for every
    host (the app's ui_definitions.cpp is held to the same cases by its own test)."""
    import copy
    import importlib.util
    with io.open(CASES, encoding="utf-8") as f:
        cases = json.load(f)
    for loader, path in LOADERS:
        spec = importlib.util.spec_from_file_location("groomlab_definitions_" + loader, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module._doc = copy.deepcopy(cases["doc"])
        for case in cases["cases"]:
            gens = dict((name, (lambda made: lambda entry: copy.deepcopy(made))(made))
                        for name, made in case.get("generators", {}).items())
            for host in ("maya", "blender", "app"):
                expected = case["expected"].get(host, case["expected"].get("*"))
                got = project(module.resolve(case["entries"], case.get("state", []), case.get("values"), gens, host=host))
                if got != expected:
                    fail("%s's definitions.py, %r on %s: %s, not %s" % (loader, case["name"], host, json.dumps(got), json.dumps(expected)))
        for case in cases["listed"]:
            for host in ("maya", "blender", "app"):
                expected = case["expected"].get(host, case["expected"].get("*"))
                got = module.listed_ids(case["entries"], host, case.get("generators", {}).items())
                if got != expected:
                    fail("%s's definitions.py, listed %r on %s: %r, not %r" % (loader, case["name"], host, got, expected))


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "engine", "ui")
    doc = load(folder)
    if not errors:
        check(doc)
        check_properties(doc)
        check_core(doc)
        check_loaders()
    for message in errors:
        print("FAIL " + message)
    print("%d problem%s in %s" % (len(errors), "" if len(errors) == 1 else "s", folder))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
