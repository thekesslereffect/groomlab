# SPDX-License-Identifier: Apache-2.0
"""Expressions: what they can read (for the editor's completion and help), and a library of
them to reuse across scenes.

An Expression node keeps its text and its own variables in the groom's graph, so they are
saved with the scene. The library is for reuse: a saved expression is a JSON file
({"name", "description", "expression", "variables", "min", "max"}) looked up, in order, in:
  1. every directory on GROOMLAB_EXPRESSION_PATH (os.pathsep-separated): a studio's shared set,
  2. the user's folder (<Maya user app dir>/groomlab/expressions), where save_expression writes,
     so what you save is there in every new scene,
  3. the examples shipped with GroomLab (<module>/expressions).
An expression in an earlier directory hides one of the same name in a later one.

    from groomlab import expressions
    expressions.list_expressions()                      # [{"name": ..., "expression": ...}, ...]
    expressions.apply_expression(groom, "expression1", "Stripes around the scalp")
    expressions.save_expression("My mask", "$y > 0.5", variables={"soft": 0.2})
"""

import json
import os
import re

import groomlab_core

from groomlab.shipped import shipped_dir as _shipped_dir  # noqa: E402
SHIPPED_DIR = _shipped_dir("expressions")

# (name, what it is). Written with a `$` in the editor; the language takes them with or without.
VARIABLES = [
    ("length", "The strand's length, along it"),
    ("longest", "The longest strand's length (the whole set)"),
    ("reach", "Root to tip, in a straight line"),
    ("straightness", "reach / length: 1 for a straight strand"),
    ("lift", "How far the strand points off the scalp: 1 straight out, 0 along it, below 0 into it"),
    ("tipx", "Tip position x"), ("tipy", "Tip position y"), ("tipz", "Tip position z"),
    ("u", "Root U on the scalp (0 without UVs)"), ("v", "Root V on the scalp (0 without UVs)"),
    ("x", "Root position x"), ("y", "Root position y"), ("z", "Root position z"),
    ("nx", "Root normal x"), ("ny", "Root normal y"), ("nz", "Root normal z"),
    ("rand", "A stable random number per strand, 0 to 1"),
    ("id", "The strand's ID (its low 24 bits): a whole number, stable as strands come and go"),
    ("faceId", "The scalp triangle the root sits on"),
    ("index", "The strand's place in the set, 0 to 1"),
    ("t", "Along the strand: 0 at the root to 1 at the tip, by length (per point)"),
    ("pt", "Along the strand: 0 at the root to 1 at the tip, by point (per point)"),
    ("px", "The point's position x (per point)"), ("py", "The point's position y (per point)"),
    ("pz", "The point's position z (per point)"),
    ("pointIndex", "The point's index on its strand (per point)"),
    ("frame", "The frame evaluated (also F); an expression reading it changes from frame to frame"),
    ("F", "The frame evaluated (as frame)"),
    ("time", "The time evaluated, in seconds"),
    ("P", "Vector: the root's position [x, y, z] (P.x, P.y, P.z or P[0]...)"),
    ("N", "Vector: the root's normal [nx, ny, nz]"),
    ("Pref", "Vector: the root on the scalp's rest pose (P without one)"),
    ("tip", "Vector: the tip's position [tipx, tipy, tipz]"),
    ("pointP", "Vector: the point's position [px, py, pz] (per point)"),
    ("pi", "3.14159..."),
]
# Channels most grooms carry (any strand channel reads by its name).
CHANNELS = [
    ("clumpId", "The strand's clump (Clump writes it)"),
    ("guideInfluence", "How close the strand is to its guides (Interpolate writes it)"),
    ("flyaway", "1 for stray hairs (Flyaway writes it)"),
    ("melanin", "The strand's pigment (HairColor writes it)"),
]
# (name, arguments, what it does). SeExpr's builtins, as XGen has them (web docs: Expressions): the
# core's own list (expression.cpp kFns), so a builtin added there is offered here too.
FUNCTIONS = [(f["name"], f["arguments"], f["about"]) for f in groomlab_core.expression_functions()]
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def completions(variables=None, channels=()):
    """(text to insert, what it is) for the editor: the node's variables, the built-in ones,
    channels, then functions."""
    out = []
    for name, value in (variables or {}).items():
        if isinstance(value, (list, tuple)):
            out += [("$%s.%s" % (name, a), "Your vector's %s" % a) for a in "xyz"]
        elif isinstance(value, dict):
            out.append(("$" + name, "Your keyable variable (ctl_%s on the groom)" % value.get("control", "")))
        else:
            out.append(("$" + name, "Your variable (%g)" % value))
    out += [("$" + name, text) for name, text in VARIABLES]
    known = {n for n, _ in CHANNELS}
    out += [("$" + name, text) for name, text in CHANNELS]
    out += [("$" + name, "A channel of the strands") for name in channels if name not in known]
    out += [("%s(" % name, "%s(%s): %s" % (name, args, text)) for name, args, text in FUNCTIONS]
    return out


def check(text, variables=None, any_type=False):
    """What is wrong with an expression ("" when nothing), with the node's own variables; the
    plugin compiles it, so the editor can say so as you type, connected or not. any_type: a
    vector does too (the Expression node's: a vector expression is a color), else it must be a number."""
    from maya import cmds
    try:
        return cmds.groomLabExpression(check=text, variables=json.dumps(check_variables(variables or {})),
                                       anyType=bool(any_type)) or ""
    except ValueError as e:
        return str(e)


def error_place(message):
    """(line, column), 1-based, that an error message points at, or None."""
    m = re.search(r"line (\d+), column (\d+)", message)
    if m:
        return int(m.group(1)), int(m.group(2))
    m = re.search(r"at position (\d+)", message)
    return (1, int(m.group(1))) if m else None


# ---------------------------------------------------------------------------
# The library

def user_dir():
    import groomlab
    return groomlab.library_user_dir("GROOMLAB_USER_EXPRESSIONS", "expressions")


def search_dirs():
    import groomlab
    return groomlab.library_dirs("GROOMLAB_EXPRESSION_PATH", user_dir, SHIPPED_DIR)


def _file_name(name):
    return re.sub(r"[^A-Za-z0-9_ -]+", "", name).strip().replace(" ", "_") or "expression"


def list_expressions():
    """Every expression in the library, sorted by name; each a dict with "name",
    "description", "expression", "variables", "min", "max", "path" and "shipped"."""
    found = {}
    for d in search_dirs():
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith(".json"):
                continue
            path = os.path.join(d, f)
            try:
                with open(path, encoding="utf-8") as fh:
                    data = json.load(fh)
            except (IOError, OSError, ValueError):
                continue
            name = data.get("name") or f[:-5]
            if name in found:
                continue  # hidden by an earlier directory
            found[name] = dict(name=name, description=data.get("description", ""),
                               expression=data.get("expression", ""), variables=data.get("variables") or {},
                               min=float(data.get("min", 0.0)), max=float(data.get("max", 1.0)),
                               path=path.replace("\\", "/"), shipped=os.path.normpath(d) == SHIPPED_DIR)
    return sorted(found.values(), key=lambda e: (e["shipped"], e["name"].lower()))


def find_expression(name):
    for e in list_expressions():
        if e["name"] == name:
            return e
    raise ValueError("no expression named %r in the library (%s)" % (name, ", ".join(search_dirs())))


def control_name(node, variable):
    """The control (the groom attribute ctl_<name>) a keyable variable of `node` reads."""
    import groomlab_core
    return groomlab_core.control_name("%s_%s" % (node, variable))  # (the core's rule, every host's)


def check_variables(variables):
    """Variables as the node stores them: {name: number, [x, y, z], or a keyable number
    {"value": v, "control": name}}; raises ValueError."""
    out = {}
    for name, value in (variables or {}).items():
        if not NAME.match(name):
            raise ValueError("%r is not a variable name (letters, digits and _, not starting with a digit)" % name)
        if isinstance(value, (list, tuple)):
            if len(value) != 3:
                raise ValueError("$%s: a vector has three numbers" % name)
            out[name] = [float(c) for c in value]
        elif isinstance(value, dict):
            control = value.get("control") or ""
            if control and not NAME.match(control):
                raise ValueError("$%s: %r is not a control name" % (name, control))
            out[name] = {"value": float(value.get("value", 0.0)), "control": control}
        else:
            out[name] = float(value)
    return out


def save_expression(name, expression, variables=None, description="", minimum=0.0, maximum=1.0, overwrite=False):
    """Save an expression to the user's library (available in every scene); returns the path."""
    if not name.strip():
        raise ValueError("an expression in the library needs a name")
    folder = user_dir()
    if not os.path.isdir(folder):
        os.makedirs(folder)
    path = os.path.join(folder, _file_name(name) + ".json")
    if os.path.exists(path) and not overwrite:
        raise ValueError("the library has an expression named %r" % name)
    data = {"name": name.strip(), "description": description, "expression": expression,
            "variables": check_variables(variables), "min": float(minimum), "max": float(maximum)}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return path.replace("\\", "/")


def delete_expression(name):
    """Remove an expression from the user's library (shipped examples stay)."""
    entry = find_expression(name)
    if entry["shipped"]:
        raise ValueError("%s ships with GroomLab and stays" % name)
    os.remove(entry["path"])


def node_values(groom, node):
    """The Expression node's text, variables, min and max, from the groom's graph."""
    from groomlab import get_graph
    for n in get_graph(groom).get("nodes", []):
        if n.get("name") == node:
            params = n.get("params", {})
            try:
                variables = json.loads(params.get("variables") or "{}")
            except ValueError:
                variables = {}
            return {"expression": params.get("expression", "$length / $longest"), "variables": variables,
                    "min": float(params.get("min", 0.0)), "max": float(params.get("max", 1.0))}
    raise ValueError("graph has no node named %r" % node)


def params_for(entry, node=None):
    """An expression (a library entry or node_values) as the Expression node's params; a keyable
    variable reads `node`'s control of its name."""
    variables = check_variables(entry.get("variables"))
    for name, value in variables.items():
        if isinstance(value, dict) and node:
            value["control"] = control_name(node, name)
    return {"expression": entry["expression"], "variables": json.dumps(variables),
            "min": float(entry.get("min", 0.0)), "max": float(entry.get("max", 1.0))}


def apply_expression(groom, node, name):
    """Load library expression `name` into the groom's Expression node `node` (one undo step)."""
    import groomlab
    entry = find_expression(name)
    with groomlab.undo_chunk("expression"):
        for key, value in params_for(entry, node=node).items():
            groomlab.set_param(groom, node, key, value)
    return entry
