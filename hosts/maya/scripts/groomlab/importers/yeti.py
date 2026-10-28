# SPDX-License-Identifier: Apache-2.0
"""Yeti grooms into GroomLab grooms, inside Maya, without Yeti's plugin or license.

    import groomlab
    groomlab.yeti_nodes()                   # [{"name", "node", "scalp", "grooms"}]
    report = groomlab.import_yeti()         # every pgYetiMaya node; or nodes=["hair"], roots="ROOT=D:/proj"
    print(report)                           # what mapped, what was approximated, what was skipped
    report.grooms[0]["groom"]               # the new groom shape

Yeti's nodes keep their graph and grooms as data Maya keeps for a node it doesn't know: the
scene's Yeti nodes are written to a Maya ASCII file (Maya writes an unknown node as it read it)
and read as text by importers/yeti_format.py; with Yeti loaded, the same export writes Yeti's own
data. A scene saved as .mb without Yeti can't be read that way (Maya refuses to write unknown
binary data as text): the error says so.

One GroomLab groom per fibre chain of each pgYetiMaya graph: from the graph's root (its shader)
back along each node's first input to the Scatter that makes the fibres (a Merge's inputs are
chains of their own). The chain's nodes become a ForeignGroom (importers/foreign.py) that
foreign.build_groom makes:

    Scatter        Scatter density (density x densityMultiplier, its map), seed; the mesh it
                   scatters on is the scalp
    Grow, Comb     Interpolate on guides from the groom they read: each groom strand a guide, each
                   painted groom attribute a guide layer; lengthMin/lengthMax/lengthMultiplier a
                   Transform scale (with scaleRandom)
    Guide          Interpolate on the guide curves it reads
    Clumping       Clump: weight as strength, attraction base/tip/bias as the profile, the clumps
                   from what feeds its second input (a Scatter: that many clumps; a Convert of the
                   groom: a clump at each guide)
    Scraggle       Noise (amount, root/tip as the profile, frequency, percent as the fraction)
    Width          Width (base and tip widths as the profile, widthMultiplier, widthRandom)
    Displace, Length, Cut   Displace, Transform scale, Length keep
    Shader         HairColor's color (aiStandardHair on the node: melanin)
    Texture        a map: the file found with the node's image search path (.isp), the Roots
                   given and environment variables, copied into the scene's masks folder
    Expression     GroomLab expressions (SeExpr both): a product of maps and along-the-fibre
                   curves is split into a map and the node's profile; anything else passes through
    Attribute      the groom attribute it reads, as the guide layer of that name

web/content/docs/importing.mdx has the table and what is approximated.
"""

import math
import os
import re
import shutil
import tempfile
import zlib

from maya import cmds

from groomlab.importers import yeti_format as yf
from groomlab.importers.foreign import ForeignBranch, ForeignGroom, ForeignOp, ImportReport, MapSource, build_groom, \
    ramp_keys
from groomlab.importers.paths import PathResolver, parse_roots
from groomlab.importers import seexpr

PLUGIN = "pgYetiMaya"
# Not calibrated (no Yeti here to compare with): Yeti's scraggle amount as a share of the fibre's
# length, its frequency in features per fibre; XGen Noise's measured factors are used.
SCRAGGLE_AMPLITUDE = 0.05
SCRAGGLE_FREQUENCY = 4.0
DEFAULT_CLUMP_COUNT = 300
# A texture's channels param: which of the image's channels it reads.
TEXTURE_CHANNELS = {0: "luminance", 1: "r", 2: "g", 3: "b", 4: "a"}
# Yeti's expression variables in GroomLab's terms ($strandu along the fibre, $s $t the root's UV).
EXPRESSION_VARIABLES = {"strandu": "t", "s": "u", "t": "v", "id": "id", "N": "N", "P": "P", "Pref": "Pref",
                        "F": "frame", "frame": "frame", "time": "time", "pi": "pi", "PI": "pi"}
# Nodes that make no fibres and no shape: skipped quietly inside a chain.
_QUIET = ("attribute",)


def yeti_available():
    """Whether Yeti's plugin is loaded (never needed: the import reads the scene data)."""
    return bool(cmds.pluginInfo(PLUGIN, query=True, loaded=True))


def _real_type(node):
    kind = cmds.nodeType(node)
    if kind in ("unknown", "unknownDag", "unknownTransform"):
        try:
            return cmds.unknownNode(node, query=True, realClassName=True) or kind
        except RuntimeError:
            return kind
    return kind


def _yeti_shapes(type_):
    found = []
    if type_ in (cmds.allNodeTypes() or []):
        found += cmds.ls(type=type_, long=True) or []
    for n in cmds.ls(type=("unknownDag", "unknown"), long=True) or []:
        if _real_type(n) == type_ and n not in found:
            found.append(n)
    return found


def _connected(node, attr, type_=None):
    try:
        out = cmds.listConnections("%s.%s" % (node, attr), source=True, destination=False, shapes=True) or []
    except (RuntimeError, ValueError):
        return []
    if type_:
        out = [n for n in out if cmds.nodeType(n) == type_ or _real_type(n) == type_]
    return out


def yeti_nodes():
    """The scene's Yeti graphs (pgYetiMaya nodes, known to Maya or not): [{"name" (the transform),
    "node" (the shape), "scalp" (the first mesh it takes), "grooms" (the pgYetiGroom nodes it
    takes)}]."""
    out = []
    for shape in _yeti_shapes("pgYetiMaya"):
        parents = cmds.listRelatives(shape, parent=True, fullPath=True) or [shape]
        meshes = _connected(shape, "ing", "mesh")
        grooms = _connected(shape, "icd")
        out.append({"name": parents[0].split("|")[-1], "node": shape, "scalp": meshes[0] if meshes else "",
                    "grooms": grooms})
    return out


def read_scene_records():
    """({short name: MaNode} of the scene's Yeti nodes, how they were read): the Yeti nodes
    exported to a temporary .ma and read back; the scene file itself when that fails and it is a
    .ma. RuntimeError when neither works (an .mb saved with Yeti, opened without it)."""
    shapes = _yeti_shapes("pgYetiMaya") + _yeti_shapes("pgYetiGroom")
    if not shapes:
        return {}, "no Yeti nodes"
    folder = tempfile.mkdtemp(prefix="groomlab_yeti_")
    problem = ""
    try:
        path = os.path.join(folder, "yeti_nodes.ma").replace("\\", "/")
        selection = cmds.ls(selection=True, long=True) or []
        try:
            cmds.select(shapes, replace=True)
            cmds.file(path, force=True, exportSelected=True, type="mayaAscii", constructionHistory=False,
                      channels=False, constraints=False, expressions=False, shader=False, preserveReferences=True)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                nodes, _ = yf.read_ma(f.read())
            if nodes:
                return nodes, "the scene's Yeti nodes, exported as Maya ASCII"
        except RuntimeError as e:
            problem = str(e).strip()
        finally:
            if selection:
                cmds.select(selection, replace=True)
            else:
                cmds.select(clear=True)
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    scene = cmds.file(query=True, sceneName=True) or ""
    if scene.lower().endswith(".ma") and os.path.isfile(scene):
        with open(scene, "r", encoding="utf-8", errors="replace") as f:
            nodes, _ = yf.read_ma(f.read())
        return nodes, "the scene file as saved (%s)" % os.path.basename(scene)
    raise RuntimeError("the Yeti nodes' data can't be read without Yeti%s: Maya keeps an unknown node's data as it "
                       "read it, and writes a binary scene's (.mb) only as binary. Save the scene as Maya ASCII "
                       "(.ma) where Yeti is installed, then import it here" % (" (%s)" % problem if problem else ""))


def import_yeti(nodes=None, roots=None, search_dirs=None, max_guides=None, map_resolution=1024, verbose=True,
                progress=None):
    """Import Yeti graphs (names or nodes from yeti_nodes(); None: all) as GroomLab grooms,
    without Yeti.

    `roots`: {"VAR": folder} for variables the image search path or a texture's path needs that
    nothing sets (or the Roots field's text, "VAR=folder" lines and plain folders); `search_dirs`:
    more folders to look for files in. `max_guides`: the most guides a groom gives (default: every
    groom strand). `progress(i, count, name)`: called before each graph (the window's status). Returns
    the ImportReport (report.grooms[i]["groom"] is each new groom), printed with `verbose`."""
    if isinstance(roots, str):
        roots, extra = parse_roots(roots)
        search_dirs = list(search_dirs or []) + extra
    found = yeti_nodes()
    if nodes is not None:
        wanted = [nodes] if isinstance(nodes, str) else list(nodes)
        by_name = {}
        for d in found:
            by_name[d["name"]] = d
            by_name[d["node"]] = d
            by_name[d["node"].split("|")[-1]] = d
        chosen = []
        for w in wanted:
            d = by_name.get(w)
            if d is None:
                raise ValueError("no Yeti node named %r (the scene has: %s)" %
                                 (w, ", ".join(sorted(x["name"] for x in found)) or "none"))
            chosen.append(d)
        found = chosen
    report = ImportReport()
    if not found:
        if verbose:
            print("The scene has no Yeti nodes.")
        return report
    records, how = read_scene_records()
    base = _base_resolver(roots, search_dirs)
    for index, d in enumerate(found):
        if progress is not None:
            progress(index, len(found), d["name"])
        short = d["node"].split("|")[-1]
        record = records.get(short)
        if record is None:
            report.begin(d["name"], "Yeti")
            report.skipped("graph", "its data was not found in %s" % how)
            continue
        try:
            reader = YetiReader(d, record, records, base, max_guides=max_guides)
            chains = reader.chains()
        except Exception as e:
            report.begin(d["name"], "Yeti")
            report.skipped("graph", "can't be read: %s" % e)
            continue
        if not chains:
            report.begin(d["name"], "Yeti")
            reader.report = report
            report.skipped("graph", "no fibres: nothing from a Scatter (or a Convert) reaches the graph's output")
            reader.note_unused([])
            continue
        first = None
        for i, chain in enumerate(chains):
            label = d["name"] if len(chains) == 1 else "%s_%d" % (d["name"], i + 1)
            entry = report.begin(label, "Yeti %s" % short)
            first = first or entry
            reader.report = report
            try:
                foreign = reader.foreign(chain, label)
                if foreign is None:
                    continue
                groom = build_groom(foreign, report, map_resolution=map_resolution)
                reader.after_build(groom, foreign)
            except Exception as e:
                report.skipped("chain", "import failed: %s" % e)
                if verbose:
                    import traceback
                    traceback.print_exc()
        last, report._current = report._current, first
        reader.note_unused(chains)
        report._current = last
    if verbose:
        print(report.text())
    return report


def _base_resolver(roots, search_dirs):
    import groomlab
    scene = groomlab.scene_folder()
    project = cmds.workspace(query=True, rootDirectory=True).replace("\\", "/").rstrip("/")
    dirs = [d for d in [scene, project, project + "/sourceimages"] if d] + list(search_dirs or [])
    return PathResolver(tokens={}, roots=roots, search_dirs=dirs)


# ------------------------------------------------------------------------------------------------
# Values: a param's value as a constant x maps (per root) x a profile (along the fibre)


class Value(object):
    """A Yeti param's value in GroomLab's terms: `const` x every MapSource in `factors` (per root;
    an expression one may vary along the fibre: its `per_point`) x `profile` ([(position, value)]
    along the fibre, `linear` when every span is). `ok` False: it could not be read (`why`)."""

    def __init__(self, const=1.0, factors=None, profile=None, linear=True, ok=True, why=""):
        self.const = const
        self.factors = list(factors or [])
        self.profile = profile
        self.linear = linear
        self.ok = ok
        self.why = why

    def times(self, other):
        if not self.ok or not other.ok:
            return Value(ok=False, why=self.why or other.why)
        profile, linear = self.profile, self.linear
        if other.profile:
            profile = multiply_profiles(profile, other.profile) if profile else other.profile
            linear = linear and other.linear if self.profile else other.linear
        return Value(self.const * other.const, self.factors + other.factors, profile, linear)

    @property
    def constant(self):
        return self.ok and not self.factors and not self.profile

    @property
    def per_point(self):
        return any(getattr(f, "per_point", False) for f in self.factors)


def multiply_profiles(a, b):
    """Two ramps' product, sampled (linear keys)."""
    a = sorted(a)
    b = sorted(b)
    ts = sorted(set([round(p, 4) for p, _ in a] + [round(p, 4) for p, _ in b] + [i / 10.0 for i in range(11)]))
    return [(t, seexpr.ramp_at(a, t) * seexpr.ramp_at(b, t)) for t in ts]


def _fmt(x):
    return ("%.6g" % x) if abs(x - round(x)) > 1e-9 or abs(x) >= 1e6 else str(int(round(x)))


def factor_text(source):
    """A MapSource as GroomLab expression text (an expression's range applied)."""
    if source.kind == "expression":
        lo, hi = source.value_range
        if (lo, hi) == (0.0, 1.0):
            return "(%s)" % source.expression
        if lo == 0.0:
            return "(%s * (%s))" % (_fmt(hi), source.expression)
        return "(%s + %s * (%s))" % (_fmt(lo), _fmt(hi - lo), source.expression)
    if source.kind == "layer":
        return 'map("%s")' % source.name
    if source.channel and source.channel not in ("luminance", "rgb"):
        return 'map("%s", "%s")' % (source.path, source.channel)
    return 'map("%s")' % source.path


def _maps_of(source):
    if source.kind == "expression":
        return list(source.maps)
    return [source]


def combine(factors, scale=1.0, name="", what=""):
    """One MapSource for a product of factors times `scale`: the map itself when it is one map
    and scale 1, else an expression of the product whose range is (0, scale) (GroomLab reads
    min + (max - min) x the expression, and samples Scatter's density up to max)."""
    if len(factors) == 1 and factors[0].kind != "expression" and abs(scale - 1.0) < 1e-9:
        return factors[0]
    if len(factors) == 1 and factors[0].kind == "expression":
        lo, hi = factors[0].value_range
        text, value_range = factors[0].expression, (lo * scale, hi * scale)
    else:
        text, value_range = " * ".join(factor_text(f) for f in factors) or "1", (0.0, scale)
    maps = []
    for f in factors:
        for m in _maps_of(f):
            if m not in maps:
                maps.append(m)
    out = MapSource("expression", expression=text, maps=maps, name=name, source=what, value_range=value_range)
    out.per_point = any(getattr(f, "per_point", False) for f in factors)
    return out


def _split_top(text, sep):
    """`text` split at `sep` outside parentheses, brackets and quotes."""
    parts, depth, quote, start = [], 0, None, 0
    for i, c in enumerate(text):
        if quote:
            if c == quote:
                quote = None
        elif c in "'\"":
            quote = c
        elif c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts]


def _strip_parens(text):
    t = text.strip()
    while t.startswith("(") and t.endswith(")"):
        depth, ok = 0, True
        for i, c in enumerate(t):
            depth += c == "("
            depth -= c == ")"
            if depth == 0 and i < len(t) - 1:
                ok = False
                break
        if not ok:
            break
        t = t[1:-1].strip()
    return t


# ------------------------------------------------------------------------------------------------
# The reader


class YetiReader(object):
    """One pgYetiMaya node's graph read into ForeignGrooms (one per fibre chain)."""

    def __init__(self, info, record, records, base_resolver, max_guides=None):
        self.info = info
        self.record = record
        self.records = records
        self.max_guides = max_guides
        self.report = ImportReport()
        self.graph = yf.parse_graph(yf.decode_graph(record.attr_text("pds") or ""))
        self.used = set()
        self._values = {}
        self._textures = {}
        self._grooms = {}
        isp = (record.attr_text("isp") or "").replace("\\", "/")
        self.search_path = [p.strip() for p in isp.split(";") if p.strip()]
        expanded = []
        for p in self.search_path:
            e, unset = base_resolver.expand(p)
            if not unset:
                expanded.append(e)
        self.resolver = PathResolver(base_resolver.tokens, base_resolver.roots,
                                     expanded + base_resolver.search_dirs, base_resolver.environ)
        self.layer_names = set()
        self.layer_defaults = {}
        self.groom = None          # the GroomStrands the guides come from
        self.groom_name = ""
        self.foreign_groom = None
        self.notes = []            # (section, what, detail) found walking the graph: in every chain's report

    # -- graph walking ---------------------------------------------------------------------------

    def root(self):
        return self.graph.root_node()

    def chains(self):
        """The fibre chains reaching the root: lists of nodes, the fibre source first."""
        root = self.root()
        if root is None:
            raise ValueError("the graph has no root node")
        return self._chains(root, set())

    def _chains(self, node, seen):
        if node.id in seen:
            return []
        seen = seen | {node.id}
        t = node.type
        if t in ("scatter", "convert"):
            return [[node]]
        if t == "import":
            return []
        if t == "merge":
            out = []
            for i in range(len(node.inputs)):
                up = self.graph.input_node(node, i)
                if up is not None:
                    out += self._chains(up, seen)
            self.used.add(node.id)
            return out
        if t in ("switch", "blend"):
            index = self._pick_input(node)
            up = self.graph.input_node(node, index)
            self.used.add(node.id)
            self.notes.append(("approximated", "%s (%s)" % (node.name, t),
                               "input %d taken; the others are left out" % index if t == "switch" else
                               "input %d taken (the one the blend weighs most); the other is left out" % index))
            return self._chains(up, seen) if up is not None else []
        up = self.graph.input_node(node, 0)
        if up is None:
            return []
        return [c + [node] for c in self._chains(up, seen)]

    def _pick_input(self, node):
        connected = [i for i, v in enumerate(node.inputs) if v]
        if not connected:
            return 0
        if node.type == "blend":
            w = next((node.number(k) for k in ("blend", "weight", "amount") if k in node.params), 0.0)
            return connected[-1] if w >= 0.5 and len(connected) > 1 else connected[0]
        idx = next((int(node.number(k)) for k in ("index", "switch", "selection", "input") if k in node.params), 0)
        return connected[max(0, min(len(connected) - 1, idx))] if idx < len(node.inputs) else connected[0]

    def note_unused(self, chains):
        """Nodes nothing of the import read, in one report line per type."""
        for c in chains:
            for n in c:
                self.used.add(n.id)
        counts = {}
        for n in self.graph.nodes.values():
            if n.id not in self.used and n.type not in ("texture", "expression", "curve", "import"):
                counts[n.type] = counts.get(n.type, 0) + 1
        if counts:
            text = ", ".join("%d %s" % (v, k) for k, v in sorted(counts.items()))
            self.report.skipped("nodes not on the way to the output", text)

    # -- grooms and meshes -----------------------------------------------------------------------

    def _import_node(self, node, index):
        up = self.graph.input_node(node, index)
        while up is not None and up.type != "import" and up.inputs:
            up = self.graph.input_node(up, 0)
        return up if up is not None and up.type == "import" else None

    def mesh_of(self, node, index=0):
        imp = self._import_node(node, index)
        name = imp.value("geometry", "") if imp else ""
        mesh = _mesh_shape(name)
        if mesh:
            self.used.add(imp.id)
            return mesh
        meshes = _connected(self.info["node"], "ing", "mesh")
        return meshes[0] if meshes else None

    def groom_of(self, node, index=1):
        """The GroomStrands of the groom node `node`'s input `index` imports (a corrective groom
        of it instead, where one is connected), with its node name; (None, "") when none."""
        imp = self._import_node(node, index)
        if imp is None:
            return None, ""
        if int(imp.number("type", 1)) != 1:
            return None, ""  # a mesh or guide curves, not a groom
        self.used.add(imp.id)
        name = (imp.value("geometry", "") or "").split("|")[-1]
        if name in self._grooms:
            return self._grooms[name]
        record = self.records.get(name)
        if record is None or record.type != "pgYetiGroom":
            self._grooms[name] = (None, name)
            return None, name
        corrective = None
        connected = set(n.split("|")[-1] for n in self.info.get("grooms", []))
        for other in self.records.values():
            if other.type == "pgYetiGroom" and (other.attr_text("cgt") or "").split("|")[-1] == name:
                if not connected or other.name in connected:
                    corrective = other
        used = corrective or record
        strands = yf.parse_strands(used.attr_tokens("pds") or [])
        if corrective is not None:
            self.report.approximated("groom %s" % name, "its corrective groom %s's strands are the guides" %
                                     corrective.name)
        matrix = _world_matrix(used.parent)
        if matrix:
            strands.strands = [[_transform(p, matrix) for p in s] for s in strands.strands]
        self._grooms[name] = (strands, name)
        return strands, name

    # -- values ----------------------------------------------------------------------------------

    def param(self, node, name, default=1.0, context=None):
        """Param `name` of `node` as a Value."""
        p = node.params.get(name)
        if p is None:
            return Value(default)
        if p.attachment:
            return self.output_value(p.attachment)
        return self.text_value(p.value, context or node, default)

    def output_value(self, output_id):
        if output_id in self._values:
            return self._values[output_id]
        n = self.graph.node_of_output(output_id)
        if n is None:
            v = Value(ok=False, why="a param reads an output that isn't in the graph")
        else:
            self.used.add(n.id)
            v = self._node_value(n)
        self._values[output_id] = v
        return v

    def _node_value(self, n):
        if n.type == "texture":
            return Value(1.0, [self.texture(n)])
        if n.type == "curve":
            keys = yf.curve_keys(n.config)
            if not keys:
                return Value(ok=False, why="curve %s has no keys" % n.name)
            param = (n.value("parameter", "$strandu") or "").strip()
            if param.lstrip("$") == "strandu":
                return Value(1.0, profile=[(p, v) for p, v, _ in keys], linear=all(i == 1 for _, _, i in keys))
            inner = self.translate(param, n)
            if inner is None:
                return Value(ok=False, why="curve %s reads %r" % (n.name, param))
            text, maps, per_point = inner
            args = ", ".join("%s, %s, %d" % (_fmt(p), _fmt(v), i) for p, v, i in keys)
            src = MapSource("expression", expression="curve(%s, %s)" % (text, args), maps=maps, source=n.name)
            src.per_point = per_point
            return Value(1.0, [src])
        if n.type == "expression":
            return self.expression_value(n.value("expression", ""), n)
        if n.type == "attribute":
            return self.attribute_value(n.value("attribute", ""), n)
        return Value(ok=False, why="a %s node drives a param" % n.type)

    def attribute_value(self, name, node=None):
        """A groom attribute read by name: the guide layer of that name, or its default."""
        if self.groom is not None and name in self.groom.attributes:
            self.layer_names.add(name)
            return Value(1.0, [MapSource("layer", name=name, path=name, source=name)])
        default = self.layer_defaults.get(name)
        if default is None and node is not None and node.type == "attribute":
            vec = yf.to_vector(node.value("default", "")) or (1.0,)
            default = vec[0]
        if default is None:
            return Value(ok=False, why="the attribute %s is on no groom" % name)
        self.report.approximated("attribute %s" % name, "on no groom read: its default %g" % default)
        return Value(default)

    def is_attribute(self, name):
        return (self.groom is not None and name in self.groom.attributes) or name in self.layer_defaults

    def text_value(self, text, node, default=1.0):
        t = (text or "").strip()
        if not t:
            return Value(default)
        n = yf.to_number(t, None)
        if n is not None:
            return Value(n)
        if re.match(r"^\$?[A-Za-z_]\w*$", t) and self.is_attribute(t.lstrip("$")):
            return self.attribute_value(t.lstrip("$"))
        return self.expression_value(t, node)

    def expression_value(self, text, node):
        """A Yeti expression: a product split into constants, maps and along-the-fibre curves where
        it is one, else translated whole."""
        clean = seexpr.strip_comments(text or "").strip()
        clean = re.sub(r"\s+", " ", clean).strip().rstrip(";").strip()
        if not clean:
            return Value(0.0)
        assigns = re.sub(r"[=!<>]=", "", clean)
        if ";" not in clean and "=" not in assigns:
            parts = _split_top(_strip_parens(clean), "*")
            if len(parts) >= 1 and all(parts):
                product = Value(1.0)
                for part in parts:
                    v = self._factor(part, node)
                    if v is None:
                        product = None
                        break
                    product = product.times(v)
                if product is not None and product.ok:
                    return product
        inner = self.translate(clean, node)
        if inner is None:
            return Value(ok=False, why="the expression %r reads what GroomLab can't" % clean)
        expr, maps, per_point = inner
        src = MapSource("expression", expression=expr, maps=maps, source=node.name)
        src.per_point = per_point
        return Value(1.0, [src])

    def _factor(self, part, node):
        """One factor of a product as a Value, or None (not a simple factor)."""
        p = _strip_parens(part)
        n = yf.to_number(p, None)
        if n is not None:
            return Value(n)
        m = re.match(r"^\$?([fv])([0-3])$", p)
        if m and node is not None and node.type == "expression":
            if m.group(1) == "v":
                return None
            return self.param(node, "float%s" % m.group(2), 0.0)
        m = re.match(r"^clamp\((.*),\s*0(?:\.0*)?\s*,\s*1(?:\.0*)?\s*\)$", p)
        if m:
            inner = self._factor(m.group(1), node)
            if inner is not None and inner.ok and not inner.profile and inner.const == 1.0 and \
                    all(f.kind in ("file", "layer") for f in inner.factors) and len(inner.factors) <= 1:
                return inner  # a map is 0-1 already
            return None
        m = re.match(r"^\$?([A-Za-z_]\w*)$", p)
        if m and self.is_attribute(m.group(1)):
            return self.attribute_value(m.group(1))
        if "(" in p or " " in p.strip() or any(c in p for c in "+-/?:<>"):
            return None
        return None

    def translate(self, text, node):
        """(GroomLab expression text, the MapSources it reads, whether it varies along the fibre) of
        a Yeti expression, or None where it reads something GroomLab has no equivalent of."""
        maps, per_point, bad = [], [False], []

        def value_text(v):
            if not v.ok:
                bad.append(v.why)
                return "0"
            parts = [] if abs(v.const - 1.0) < 1e-12 else [_fmt(v.const)]
            for f in v.factors:
                parts.append(factor_text(f))
                for m in _maps_of(f):
                    if m not in maps:
                        maps.append(m)
                per_point[0] = per_point[0] or getattr(f, "per_point", False)
            if v.profile:
                args = ", ".join("%s, %s, %d" % (_fmt(p), _fmt(val), 1 if v.linear else 2) for p, val in v.profile)
                parts.append("curve(t, %s)" % args)
                per_point[0] = True
            return "(%s)" % (" * ".join(parts) or "1")

        def sub(m):
            word = m.group(0)
            if m.end() < len(text) and text[m.end():].lstrip().startswith("("):
                return word  # a function
            name = word.lstrip("$")
            fm = re.match(r"^([fv])([0-3])$", name)
            if fm and node is not None and node.type == "expression":
                if fm.group(1) == "f":
                    return value_text(self.param(node, "float%s" % fm.group(2), 0.0))
                vec = yf.to_vector(node.value("vector%s" % fm.group(2), "")) or (0.0, 0.0, 0.0)
                return "[%s]" % ", ".join(_fmt(c) for c in vec)
            if self.is_attribute(name):
                return value_text(self.attribute_value(name))
            if name in EXPRESSION_VARIABLES:
                g = EXPRESSION_VARIABLES[name]
                if g == "t":
                    per_point[0] = True
                return "$" + g
            bad.append(name)
            return word

        body = re.sub(r"(?<![\w.\"'])\$?[A-Za-z_]\w*", sub, text)
        if bad:
            return None
        return body.strip(), maps, per_point[0]

    def texture(self, node):
        """A texture node's MapSource (one per file and channel)."""
        name = (node.value("file_name", "") or "").replace("\\", "/").strip()
        channel = TEXTURE_CHANNELS.get(int(node.number("channels", 0)), "luminance")
        key = (name, channel)
        if key in self._textures:
            return self._textures[key]
        path = name
        if name and not os.path.isabs(name) and not name.startswith(("$", "%", "~")) and self.search_path:
            first = self.search_path[0]
            for p in self.search_path:
                if not self.resolver.expand(p)[1]:
                    first = p
                    break
            path = first.rstrip("/") + "/" + name
        stem = os.path.splitext(os.path.basename(name))[0] or "texture"
        src = MapSource("file", path=path, name=stem, channel=channel, resolver=self.resolver,
                        source="%s (texture %s)" % (node.name, name))
        what = "%s (texture)" % node.name
        u = (node.value("uCoord", "$s") or "").replace(" ", "")
        v = (node.value("vCoord", "1-$t") or "").replace(" ", "")
        if u != "$s" or v != "1-$t":
            self.report.approximated(what, "read at the root's UV; its coordinates (%s, %s) are not taken over" %
                                     (u, v))
        if re.search(r"%0?\d*d|<f>|<UDIM>|#|\$F", name):
            self.report.approximated(what, "a sequence or tiles: the file as named is read")
        self.report.mapped(what, "a map from %s%s" % (name, "" if channel == "luminance" else
                                                       " (channel %s)" % channel))
        self._textures[key] = src
        return src

    def num(self, node, name, default=0.0):
        """A param's number. One a node's output drives: that value when it is a constant, else the
        param's own value (reported)."""
        p = node.params.get(name)
        if p is None or not p.attachment:
            return node.number(name, default)
        v = self.output_value(p.attachment)
        if v.constant:
            return v.const
        self.report.approximated("%s (%s) %s" % (node.name, node.type, name),
                                 "driven per strand in Yeti: its own value %s taken" % _fmt(node.number(name, default)))
        return node.number(name, default)

    # -- assigning values to GroomLab params -----------------------------------------------------

    def assign(self, op, param, value, what, scale=1.0, profile_param=None, default=None):
        """`value` (x `scale`) into op.param: a constant, a map, an expression; its profile into
        op.profile_param. Returns False when it could not be read (reported)."""
        if not value.ok:
            self.report.skipped(what, "%s left at %s: %s" % (param, op.params.get(param, default), value.why))
            return False
        if value.profile:
            if profile_param:
                current = op.params.get(profile_param)
                keys = _ramp_points(current) if current is not None else None
                points = multiply_profiles(keys, value.profile) if keys else value.profile
                linear = value.linear if not keys else True
                op.params[profile_param] = ramp_keys(_thin(points), linear=linear)
            else:
                self.report.skipped(what, "varies along the fibre: GroomLab's %s takes its root value" % param)
        if not value.factors:
            op.params[param] = value.const * scale
            return True
        source = combine(value.factors, value.const * scale if (len(value.factors) > 1 or
                                                                 value.factors[0].kind == "expression") else 1.0,
                         name=param, what=what)
        if source.kind == "expression":
            op.params[param] = 1.0
        else:
            op.params[param] = value.const * scale
        op.maps[param] = source
        return True

    # -- a chain as a ForeignGroom ---------------------------------------------------------------

    def foreign(self, chain, label):
        for section, what, detail in self.notes:
            self.report.note(section, what, detail)
        source = chain[0]
        self.layer_names = set()
        self._picked = None
        self.guide_length = 1.0
        if source.type == "scatter":
            scalp = self.mesh_of(source, 0)
        else:  # a Convert: the groom's strands are the fibres
            scalp = self.mesh_of(source, 1)
        if not scalp:
            self.report.skipped("chain", "the mesh it grows on is not in the scene")
            return None
        self.scalp = scalp
        self.area = cmds.polyEvaluate(scalp, worldArea=True) or 0.0
        # The guides: the first groom the chain reads.
        self.groom, self.groom_name = None, ""
        for n in chain:
            index = {"grow": 1, "comb": 1, "attribute": 1, "guide": 1, "convert": 0}.get(n.type)
            if index is None:
                continue
            strands, name = self.groom_of(n, index)
            if strands is not None:
                self.groom, self.groom_name = strands, name
                break
            if name:
                self.report.skipped("groom %s" % name, "not in the scene's Yeti nodes")
        foreign = ForeignGroom(label, scalp, source="Yeti %s" % self.record.name)
        self.foreign_groom = foreign
        self.density_scale = 0.0
        if self.groom is not None:
            self._guides(foreign)
        for n in chain:
            self.used.add(n.id)
            if n.flag("ignore"):
                self.report.skipped("%s (%s)" % (n.name, n.type), "ignored in Yeti")
                continue
            handler = getattr(self, "_node_" + n.type, None)
            what = "%s (%s)" % (n.name, n.type)
            if handler is None:
                if n.type not in _QUIET:
                    self.report.skipped(what, "no GroomLab equivalent taken over")
                continue
            handler(n, foreign, what)
        color = None
        try:
            from groomlab.importers.xgen import _shader_op
            color = _shader_op(self.info["node"], self.report)
        except Exception:
            color = None
        if color is not None:
            foreign.ops = [o for o in foreign.ops if o.role != "color"] + [color]
        for name in sorted(self.layer_names):
            values = self.groom.attributes[name]
            foreign.guide_layers[name] = (values if self._picked is None else [values[i] for i in self._picked],
                                          self.layer_defaults.get(name, 1.0))
        return foreign

    def _guides(self, foreign):
        strands = [s for s in self.groom.strands if len(s) >= 2]
        index = [i for i, s in enumerate(self.groom.strands) if len(s) >= 2]
        self._picked = None
        if self.max_guides and len(strands) > self.max_guides:
            chosen = pick_spread([s[0] for s in strands], self.area, self.max_guides)
            self._picked = [index[i] for i in chosen]
            strands = [strands[i] for i in chosen]
        elif len(index) != len(self.groom.strands):
            self._picked = index
        foreign.guides = strands
        lengths = sorted(_length(s) for s in strands)
        self.guide_length = lengths[len(lengths) // 2] if lengths else 1.0
        painted = self.groom.painted()
        self.report.mapped("groom %s" % self.groom_name, "%d strands as guides%s; %d painted attribute%s" %
                           (len(strands), "" if self._picked is None or len(strands) == self.groom.count else
                            " (of %d)" % self.groom.count, len(painted), "" if len(painted) == 1 else "s"))

    # -- node handlers ---------------------------------------------------------------------------

    def _node_scatter(self, n, foreign, what):
        density = self.param(n, "density", 1.0).times(self.param(n, "densityMultiplier", 1.0))
        if not density.ok:
            self.report.skipped(what, "density: %s; density %g taken" %
                                (density.why, self.num(n, "densityMultiplier", 1.0)))
            density = Value(self.num(n, "densityMultiplier", 1.0))
        if density.profile:
            self.report.skipped(what, "density varies along the fibre (base/tip density): not taken over")
        self.density_scale = density.const
        if density.factors:
            source = combine(density.factors, density.const if (len(density.factors) > 1 or
                                                                density.factors[0].kind == "expression") else 1.0,
                             name="density", what=what + " density")
            foreign.density = 1.0 if source.kind == "expression" else density.const
            foreign.density_map = source
        else:
            foreign.density = density.const
        foreign.scatter["seed"] = int(self.num(n, "seed", 0)) % 2147483647
        self.report.mapped(what, "Scatter density %g per unit area%s, seed" %
                           (self.density_scale, " times a map" if density.factors else ""))
        for k, default in (("baseDensity", 1.0), ("tipDensity", 1.0)):
            if abs(self.num(n, k, default) - default) > 1e-6:
                self.report.skipped(what + " " + k, "scattering along curves is not taken over")
        if (n.value("faceSet", "") or "").strip():
            self.report.skipped(what + " faceSet", "the whole mesh is scattered on (a density map can limit it)")
        group = (n.value("groomAttribute", "") or "").strip()
        if group and self.groom is not None and group in self.groom.attributes:
            self.layer_names.add(group)
            layer = MapSource("layer", name=group, path=group, source=group)
            if foreign.density_map is None:
                foreign.density_map = layer
            else:
                foreign.density_map = combine([foreign.density_map, layer], name="density", what=what)
                foreign.density = 1.0 if foreign.density_map.kind == "expression" else foreign.density
            self.report.mapped(what + " groomAttribute", "the groom's %s attribute on the density" % group)

    def _node_convert(self, n, foreign, what):
        count = self.groom.count if self.groom is not None else 0
        foreign.density = count / max(self.area, 1e-9)
        self.density_scale = foreign.density
        self.report.approximated(what, "the groom's strands are the fibres: Scatter grows as many (%d) and "
                                       "Interpolate gives them the strands' shapes" % count)

    def _node_grow(self, n, foreign, what):
        lo, hi = self.num(n, "lengthMin", 1.0), self.num(n, "lengthMax", 1.0)
        mult = self.param(n, "lengthMultiplier", 1.0)
        segments = int(self.num(n, "segmentCount", 0))
        if segments:
            foreign.segments = max(8, min(48, segments))
        if self.groom is None:
            self.report.approximated(what, "grows without a groom: straight strands of Scatter's length")
        avg = (lo + hi) / 2.0
        if not mult.constant or abs(avg * mult.const - 1.0) > 1e-6 or abs(hi - lo) > 1e-6:
            op = ForeignOp("length", {"space": "strand"}, source=what)
            self.assign(op, "scale", mult, what + " lengthMultiplier", scale=avg)
            if hi > lo:
                op.params["scaleRandom"] = max(0.0, min(1.0, (hi - lo) / max(hi + lo, 1e-9)))
            foreign.ops.append(op)
            self.report.mapped(what, "the groom's lengths; lengthMin %g to lengthMax %g as a Transform scale %g, "
                                     "scaleRandom %.3g" % (lo, hi, avg, op.params.get("scaleRandom", 0.0)))
        else:
            self.report.mapped(what, "the groom's lengths (Interpolate)")
        if segments:
            self.report.approximated(what + " segmentCount", "%d segments: Resample %d" % (segments, foreign.segments))

    def _node_comb(self, n, foreign, what):
        w = self.param(n, "weight", 1.0)
        if not w.constant or abs(w.const - 1.0) > 1e-6:
            self.report.approximated(what, "comb weight %s: GroomLab's strands follow the guides fully" %
                                     (_fmt(w.const) if w.constant else "from a map"))
        else:
            self.report.mapped(what, "the guides' shapes (Interpolate)")
        if abs(self.num(n, "spread", 0.0)) > 1e-6:
            self.report.skipped(what + " spread", "not taken over")

    def _node_guide(self, n, foreign, what):
        if self.groom is None:
            imp = self._import_node(n, 1)
            curves = _curves_under(imp.value("geometry", "") if imp else "")
            if curves:
                foreign.guides = curves
                lengths = sorted(_length(s) for s in curves)
                self.guide_length = lengths[len(lengths) // 2]
                self.report.mapped(what, "%d guide curves as guides (Interpolate)" % len(curves))
            else:
                self.report.skipped(what, "its guide curves are not in the scene")
        else:
            self.report.mapped(what, "the guides (Interpolate)")
        k = int(self.num(n, "maxNumberOfGuideInfluences", 0))
        if k > 0:
            foreign.interpolate["neighbors"] = max(1, min(16, k))
        w = self.num(n, "weight", 1.0) if "weight" in n.params else self.num(n, "guideWeight", 1.0)
        if abs(w - 1.0) > 1e-6:
            self.report.approximated(what, "guide weight %g: GroomLab's strands follow the guides fully" % w)

    def _node_attribute(self, n, foreign, what):
        name = (n.value("attribute", "") or "").strip()
        groom_attr = (n.value("groomAttribute", "") or name).strip()
        vec = yf.to_vector(n.value("default", "")) or (yf.to_number(n.value("default", ""), 1.0),)
        if name:
            self.layer_defaults[name] = vec[0]
        if self.groom is not None and groom_attr in self.groom.attributes:
            if groom_attr != name and name:
                self.groom.attributes[name] = self.groom.attributes[groom_attr]
            self.report.mapped(what, "the groom's %s as the guide layer %s" % (groom_attr, name or groom_attr))
        elif name:
            self.report.approximated(what, "%s is on no groom read: its default %g wherever it is read" %
                                     (groom_attr, vec[0]))

    def _node_clumping(self, n, foreign, what):
        op = ForeignOp("clump", {}, source=what)
        self.assign(op, "strength", self.param(n, "weight", 1.0), what + " weight", profile_param="profile")
        base, tip, bias = self.num(n, "attractionBase", 1.0), self.num(n, "attractionTip", 1.0), \
            self.num(n, "attractionBias", 0.0)
        attraction = attraction_profile(base, tip, bias)
        current = op.params.get("profile")
        keys = _ramp_points(current) if current is not None else None
        op.params["profile"] = ramp_keys(_thin(multiply_profiles(keys, attraction) if keys else attraction),
                                         linear=True)
        # Yeti's clumpWeightRandom varies the weight per clump, weightRandom per strand.
        for yeti_name, ours in (("clumpWeightRandom", "strengthRandom"), ("weightRandom", "strandStrengthRandom")):
            rnd = self.num(n, yeti_name, 0.0)
            if rnd > 0:
                op.params[ours] = max(0.0, min(1.0, rnd))
        fly = self.num(n, "flyAway", 0.0)
        if fly > 0:
            seed = zlib.crc32((n.id or n.name).encode("utf-8")) % 9973
            op.maps["affect"] = MapSource("expression", expression="rand(0, 1, %d) < %s ? 0 : 1" % (seed, _fmt(fly)),
                                          name="affect", source=what)
            op.params["affect"] = 1.0
            self.report.approximated(what + " flyAway", "a share %g of the strands left out of the clump" % fly)
        self._clump_centers(n, op, what)
        foreign.ops.append(op)
        self.report.mapped(what, "Clump (weight as strength; attraction base %g, tip %g, bias %g as the profile)" %
                           (base, tip, bias))
        twist = self.num(n, "twist", 0.0)
        if abs(twist) > 1e-6:
            foreign.ops.append(ForeignOp("twist", {"turns": 0.0, "profile": op.params["profile"]},
                                         source=what + " twist", per_strand={"turns": twist}))
            self.report.approximated(what + " twist", "a Twist of %g turns along the strand about each clump, as "
                                                      "much as the strands are attracted (the clump's profile)" % twist)
        for k in ("twistRandom",):
            if abs(self.num(n, k, 0.0)) > 1e-6:
                self.report.skipped(what + " " + k, "not taken over")
        if int(self.num(n, "lengthHandling", 0)) != 0:
            self.report.skipped(what + " lengthHandling", "not taken over")
        if "clumpSize" in n.params:
            self.report.skipped(what + " clumpSize", "%s not taken over: GroomLab's clumps are as many as the clump "
                                                     "curves" % _fmt(self.num(n, "clumpSize", 0.0)))

    def _clump_centers(self, n, op, what):
        """The clump curves feeding a Clumping's second input as Clump's centers: the groom's strands
        (a Convert of the groom) as the guides, or a Scatter's fibres grown on the guides (with the
        branch's own Grow lengths and Scraggle) as a branch of their own: Clump's centers as curves,
        Yeti's clump curves."""
        up = self.graph.input_node(n, 1)
        if up is None:
            op.params.update(clumpBy="count", count=DEFAULT_CLUMP_COUNT)
            self.report.approximated(what, "no clump curves connected: %d clumps picked among the strands" %
                                     DEFAULT_CLUMP_COUNT)
            return
        if up.type == "import":
            chain = [up]
        else:
            chains = self._chains(up, set())
            chain = chains[0] if chains else [up]
        for c in chain:
            self.used.add(c.id)
        src = chain[0]
        if src.type == "convert" or (src.type == "import" and int(self.num(src, "type", 0)) == 1):
            op.params.update(centersFrom="inputCurves")
            op.inputs["centers"] = "guides"
            extra = sorted(set(c.type for c in chain[1:] if c.type not in ("attribute",)))
            if extra:
                self.report.skipped(what + " clump curves", "their own %s not taken over (the clumps follow the "
                                                            "guides as they are)" % ", ".join(extra))
            self.report.mapped(what + " clumps", "the groom's strands as the clump curves (Clump's centers: the "
                                                 "guides, as curves)")
            return
        if src.type != "scatter":
            op.params.update(clumpBy="count", count=DEFAULT_CLUMP_COUNT)
            self.report.approximated(what + " clumps", "clump curves from a %s: %d clumps picked among the strands" %
                                     (src.type, DEFAULT_CLUMP_COUNT))
            return
        branch = ForeignBranch(source=what + " clump curves")
        saved = self.density_scale
        try:
            for c in chain:
                kind = "%s (%s)" % (c.name, c.type)
                if c.flag("ignore"):
                    self.report.skipped(kind, "ignored in Yeti")
                elif c.type in ("scatter", "grow", "scraggle", "length", "cut", "displace"):
                    getattr(self, "_node_" + c.type)(c, branch, kind)
                elif c.type not in ("comb", "attribute"):
                    self.report.skipped(what + " clump curves", "their own %s is not taken over" % c.type)
        finally:
            self.density_scale = saved
        op.params.update(centersFrom="inputCurves")
        op.inputs["centers"] = branch
        self.report.mapped(what + " clumps", "the clump curves as a branch of their own: Scatter, Interpolate on "
                                             "the guides and their own nodes, as Clump's centers (curves)")

    def _node_scraggle(self, n, foreign, what):
        amount = self.param(n, "scraggle", 1.0).times(self.param(n, "scraggleMultiplier", 1.0))
        length = getattr(self, "guide_length", 1.0)
        op = ForeignOp("noise", {"space": "strand", "octaves": 2,
                                 "frequency": max(0.01, self.num(n, "scraggleFrequency", 1.0)) * SCRAGGLE_FREQUENCY,
                                 "profile": ramp_keys([(0.0, self.num(n, "rootScraggle", 0.0)),
                                                       (1.0, self.num(n, "tipScraggle", 1.0))], linear=True),
                                 "seed": len(foreign.ops) + 1},
                       source=what)
        self.assign(op, "amplitude", amount, what + " scraggle", scale=SCRAGGLE_AMPLITUDE * length,
                    profile_param="profile")
        rnd = self.num(n, "scraggleRandom", 0.0)
        if rnd > 0:
            factor = MapSource("expression", expression="1 - %s * rand(0, 1, %d)" % (_fmt(min(1.0, rnd)),
                                                                                      len(foreign.ops) + 7),
                               source=what)
            current = op.maps.get("amplitude")
            factors = ([current] if current is not None else []) + [factor]
            scale = op.params.get("amplitude", 0.0) if current is None or current.kind != "expression" else 1.0
            op.maps["amplitude"] = combine(factors, scale, name="amplitude", what=what)
            op.params["amplitude"] = 1.0
        percent = self.num(n, "percent", 1.0)
        if percent < 1.0:
            op.params["fraction"] = max(0.0, percent)
        foreign.ops.append(op)
        self.report.approximated(what, "Noise: amplitude = scraggle x multiplier x %g x the guides' median length "
                                       "%.4g; frequency %g x %g per strand length; root/tip scraggle as the "
                                       "profile; percent as the fraction (not calibrated against Yeti)" %
                                 (SCRAGGLE_AMPLITUDE, length, self.num(n, "scraggleFrequency", 1.0),
                                  SCRAGGLE_FREQUENCY))
        for k, default in (("space", 0.0), ("distribution", 0.0)):
            if abs(self.num(n, k, default) - default) > 1e-6:
                self.report.skipped(what + " " + k, "not taken over")

    def _node_width(self, n, foreign, what):
        w0, w1 = self.num(n, "baseWidth", 0.01), self.num(n, "tipWidth", 0.01)
        p0, p1 = self.num(n, "baseParameter", 0.0), self.num(n, "tipParameter", 1.0)
        tip = w1 / w0 if w0 > 0 else 1.0
        keys = [(0.0, 1.0)]
        if p0 > 0:
            keys.append((p0, 1.0))
        keys.append((max(p0, min(1.0, p1)), tip))
        if p1 < 1.0:
            keys.append((1.0, tip))
        op = ForeignOp("width", {"width": w0, "profile": ramp_keys(_thin(keys), linear=True)}, source=what)
        self.assign(op, "width", self.param(n, "widthMultiplier", 1.0), what + " widthMultiplier", scale=w0,
                    profile_param="profile")
        if self.num(n, "widthRandom", 0.0) > 0:
            op.params["randomness"] = max(0.0, min(1.0, self.num(n, "widthRandom", 0.0)))
        for i, existing in enumerate(foreign.ops):
            if existing.role == "width":
                foreign.ops[i] = op
                self.report.approximated(what, "replaces an earlier Width")
                break
        else:
            foreign.ops.append(op)
        self.report.mapped(what, "Width %g at the root, %g at the tip (the profile), widthRandom as randomness" %
                           (w0, w1))

    def _node_displace(self, n, foreign, what):
        name = next((k for k in ("displacement", "amount", "magnitude", "distance") if k in n.params), None)
        if name is None:
            self.report.skipped(what, "no displacement amount found")
            return
        amount = self.param(n, name, 0.0)
        mult = next((self.num(n, k, 1.0) for k in ("displacementMultiplier", "multiplier") if k in n.params), 1.0)
        op = ForeignOp("displace", {"along": "normal"}, source=what)
        root, tip = self.num(n, "rootDisplacement", 1.0), self.num(n, "tipDisplacement", 1.0)
        op.params["profile"] = ramp_keys([(0.0, root), (1.0, tip)], linear=True)
        self.assign(op, "distance", amount, what + " " + name, scale=mult, profile_param="profile")
        foreign.ops.append(op)
        self.report.approximated(what, "Displace along the scalp's normal: %s x %g" % (name, mult))

    def _node_length(self, n, foreign, what):
        name = next((k for k in ("length", "lengthMultiplier", "multiplier", "scale") if k in n.params), None)
        if name is None:
            self.report.skipped(what, "no length found")
            return
        op = ForeignOp("length", {"space": "strand"}, source=what)
        self.assign(op, "scale", self.param(n, name, 1.0), what + " " + name)
        foreign.ops.append(op)
        self.report.approximated(what, "a Transform scale of the strands' length (%s)" % name)

    def _node_cut(self, n, foreign, what):
        name = next((k for k in ("amount", "cut", "percent") if k in n.params), None)
        if name is None:
            self.report.skipped(what, "no cut amount found")
            return
        amount = self.param(n, name, 0.0)
        op = ForeignOp("cut", {"keep": 1.0}, source=what)
        if amount.constant:
            op.params["keep"] = max(0.0, min(1.0, 1.0 - amount.const))
        else:
            source = combine(amount.factors, amount.const, what=what)
            text = "max(0, 1 - %s)" % factor_text(source)
            op.maps["keep"] = MapSource("expression", expression=text, maps=_maps_of(source), name="cut", source=what)
        foreign.ops.append(op)
        self.report.approximated(what, "Length keep 1 - %s (a share of each strand cut off)" % name)

    def _node_direction(self, n, foreign, what):
        self.report.approximated(what, "the guides set the strands' direction (Interpolate)")

    def _node_shader(self, n, foreign, what):
        if not n.flag("doColor", True):
            self.report.skipped(what, "doColor off")
            return
        color = yf.to_vector(n.value("diffuseColor", "")) or (0.8, 0.8, 0.8)
        foreign.ops.append(ForeignOp("color", {"mode": "color", "color": list(color)}, source=what))
        self.report.approximated(what, "HairColor color %s (Yeti's shader color)" %
                                 ", ".join("%.3g" % c for c in color))
        for k in ("doHsv", "doShift", "doOcclusion", "doOpacity"):
            if n.flag(k, False):
                self.report.skipped(what + " " + k, "not taken over")

    # -- once the groom is built -----------------------------------------------------------------

    def after_build(self, groom, foreign):
        """The strand count the groom grows against Yeti's density x the scalp's area (reported),
        and the groom's evaluation errors."""
        from groomlab.groom import errors
        from groomlab.review import groom_stats
        preview = cmds.getAttr(groom + ".previewDensity")
        cmds.setAttr(groom + ".previewDensity", 1.0)
        try:
            count = groom_stats(groom)["strands"]
            expected = self.density_scale * self.area
            share = count / expected if expected > 0 and foreign.density_map is not None else 1.0
            if expected > 0:
                self.report.approximated("strands", "GroomLab grows %d: Yeti's density %g x the scalp's area %.4g = "
                                                    "%d before the density map%s" %
                                         (count, self.density_scale, self.area, int(round(expected)),
                                          " (the map keeps %.0f%%)" % (100 * share) if foreign.density_map else ""))
            for message in errors(groom):
                self.report.skipped("evaluation", message)
        finally:
            cmds.setAttr(groom + ".previewDensity", preview)


# ------------------------------------------------------------------------------------------------
# Helpers


def attraction_profile(base, tip, bias, samples=6):
    """Clumping's attraction along the fibre (base to tip, bias -1..1 holding it near the base
    (negative) or the tip (positive)) as linear keys."""
    exponent = 2.0 ** (-2.0 * max(-1.0, min(1.0, bias)))
    return [(i / float(samples - 1), base + (tip - base) * (i / float(samples - 1)) ** exponent)
            for i in range(samples)]


def pick_spread(roots, area, target, seed=1):
    """About `target` (at most) indices of `roots` spread over the scalp, each at least a spacing
    apart: the spacing from the scalp's area, made smaller while too few are picked (roots on a part
    of the scalp)."""
    spacing = 0.8 * math.sqrt(max(area, 1e-9) / max(1, target))
    chosen = []
    for _ in range(12):
        chosen = _spread(roots, spacing, seed)
        if len(chosen) >= 0.8 * target:
            break
        spacing *= 0.8
    if len(chosen) > target:
        import random
        chosen = sorted(random.Random(seed).sample(chosen, target))
    return chosen


def _spread(roots, spacing, seed):
    import random
    grid, chosen = {}, []
    order = list(range(len(roots)))
    random.Random(seed).shuffle(order)
    for i in order:
        p = roots[i]
        key = tuple(int(math.floor(c / spacing)) for c in p)
        near = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        q = roots[j]
                        if sum((a - b) ** 2 for a, b in zip(p, q)) < spacing * spacing:
                            near = True
                            break
                    if near:
                        break
                if near:
                    break
            if near:
                break
        if not near:
            grid.setdefault(key, []).append(i)
            chosen.append(i)
    return sorted(chosen)


def _ramp_points(value):
    if isinstance(value, dict):
        value = value.get("keys", [])
    return [(float(p), float(v)) for p, v in value] if value else None


def _thin(points, tolerance=1e-3):
    if len(points) <= 2:
        return points
    out = [points[0]]
    for b, c in zip(points[1:], points[2:]):
        t = (b[0] - out[-1][0]) / max(1e-9, c[0] - out[-1][0])
        if abs(out[-1][1] + (c[1] - out[-1][1]) * t - b[1]) > tolerance:
            out.append(b)
    out.append(points[-1])
    return out


def _length(points):
    return sum(math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q))) for p, q in zip(points, points[1:]))


def _mesh_shape(name):
    if not name:
        return None
    for node in cmds.ls(name, long=True) or cmds.ls("*:" + name.split(":")[-1], long=True) or []:
        if cmds.nodeType(node) == "mesh":
            return node
        shapes = cmds.listRelatives(node, shapes=True, noIntermediate=True, fullPath=True, type="mesh") or []
        if not shapes:
            shapes = cmds.listRelatives(node, allDescendents=True, noIntermediate=True, fullPath=True,
                                        type="mesh") or []
        if shapes:
            return shapes[0]
    return None


def _curves_under(name):
    """Every NURBS curve under a node, as point lists (world space)."""
    out = []
    if not name:
        return out
    for node in cmds.ls(name, long=True) or []:
        shapes = [node] if cmds.nodeType(node) == "nurbsCurve" else \
            (cmds.listRelatives(node, allDescendents=True, fullPath=True, type="nurbsCurve") or [])
        for s in shapes:
            if cmds.getAttr(s + ".intermediateObject"):
                continue
            n = cmds.getAttr(s + ".spans") + cmds.getAttr(s + ".degree")
            pts = [tuple(cmds.pointPosition("%s.cv[%d]" % (s, i), world=True)) for i in range(n)]
            if len(pts) >= 2:
                out.append(pts)
    return out


def _world_matrix(transform):
    if not transform or not cmds.objExists(transform):
        return None
    m = cmds.xform(transform, query=True, worldSpace=True, matrix=True)
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    if all(abs(a - b) < 1e-9 for a, b in zip(m, identity)):
        return None
    return m


def _transform(p, m):
    x, y, z = p
    return (x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14])
