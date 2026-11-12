# SPDX-License-Identifier: Apache-2.0
"""XGen grooms into GroomLab grooms, inside Maya: both flavors.

    import groomlab
    groomlab.xgen_descriptions()           # [{"name", "flavor": "igs" | "core", "node", "palette", "scalp"}]
    report = groomlab.import_xgen()        # every description; or descriptions=["hair", ...]
    print(report)                          # what mapped, what was approximated, what was skipped
    report.grooms[0]["groom"]              # the new groom shape

One GroomLab groom per description, on the description's scalp. The readers here turn XGen's
stack into a ForeignGroom (importers/foreign.py, the layer a Yeti importer shares) and
foreign.build_groom makes it. web/content/docs/importing.mdx has the mapping table.

Interactive Groom Splines (xgmSplineDescription): every spline is a strand, so the guides are a
spread-out subset of the splines as XGen computes them with the look modifiers (Clump, Noise, Cut,
Linear Wire) muted for the moment: what the brushes, Sculpt layers, Scale and Guide modifiers made
is baked into the guides. The look modifiers become GroomLab nodes; the strand count is matched to
XGen's (Scatter's density set from it); the width comes from the splines' own widths.

XGen Core descriptions (an xgmPalette's descriptions, read through the xgenm API, as the scene's
.xgen collection holds them): the guides are the description's guides, the generator's density and
mask become Scatter's, the Spline primitive's length a Transform scale (in the Guides method the
length multiplies the guides'), its width and taper Width's, the FX modules nodes in their order.

Maps: a Ptex map (map('${DESC}/paintmaps/<name>/')) becomes a GroomLab map named <name> from the
Ptex file, an IGS attribute's file texture a map from that file; each found once (XGen's tokens,
environment variables, the Roots given) and copied into the scene's masks folder.
"""

import math
import os
import random
import shutil
import tempfile

from maya import cmds

from groomlab.importers import seexpr
from groomlab.importers.foreign import ForeignGroom, ForeignOp, ImportReport, MapSource, build_groom, ramp_keys
from groomlab.importers.paths import PathResolver, parse_roots

PLUGIN = "xgenToolkit"
# IGS modifiers that become GroomLab nodes (muted while the guides are read), and the ones left out.
IGS_LOOK = ("xgmModifierClump", "xgmModifierNoise", "xgmModifierCut")
IGS_SKIPPED = ("xgmModifierLinearWire",)
# Calibrations, XGen units to GroomLab's, measured on matching grooms (tests/hosts/maya/test_xgen_import.py
# prints the comparison): XGen's noise magnitude is a share of the strand's length.
NOISE_AMPLITUDE_PER_MAGNITUDE = 0.05
NOISE_FREQUENCY_PER_FREQUENCY = 4.0
# An IGS Clump's pull for its clump value: XGen's splines (exported from mayapy, as a batch render
# computes them) gather their tips about as much as a GroomLab Clump of a third the strength, and
# saturate above clump 0.8.
IGS_CLUMP_STRENGTH = 0.32
# How far from its guides a Core description grows hair, in guide spacings.
GUIDE_REACH = 2.0


def xgen_available():
    """Whether XGen's plugin is (or can be) loaded."""
    if cmds.pluginInfo(PLUGIN, query=True, loaded=True):
        return True
    try:
        cmds.loadPlugin(PLUGIN, quiet=True)
        return True
    except RuntimeError:
        return False


def xgen_descriptions():
    """The scene's XGen descriptions, both flavors: [{"name", "flavor" ("igs" or "core"), "node"
    (the description's node: an IGS shape, a Core description transform), "palette" (Core), "scalp"}]."""
    out = []
    if not xgen_available():
        return out
    for shape in cmds.ls(type="xgmSplineDescription", long=True) or []:
        transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
        meshes = cmds.xgmSplineQuery(shape, listBoundMeshes=True) or []
        out.append({"name": transform.split("|")[-1], "flavor": "igs", "node": shape, "palette": "",
                    "scalp": meshes[0] if meshes else ""})
    import xgenm as xg
    for palette in xg.palettes() or ():
        for desc in xg.descriptions(palette) or ():
            geoms = xg.boundGeometry(palette, desc) or ()
            out.append({"name": desc, "flavor": "core", "node": desc, "palette": palette,
                        "scalp": geoms[0] if geoms else ""})
    return out


def import_xgen(descriptions=None, roots=None, search_dirs=None, max_guides=None, map_resolution=1024,
                match_count=True, verbose=True, progress=None):
    """Import XGen descriptions (names from xgen_descriptions(); None: all) as GroomLab grooms.

    `roots`: {"VAR": folder} for variables a map's path needs that nothing sets (or the Roots
    field's text, "VAR=folder" lines and plain folders); `search_dirs`: more folders to look for
    files in. `max_guides`: the most guides an IGS description gives (default: about one per 25
    splines, 32 to 2000). `match_count`: set Scatter's density so the groom grows as many strands
    as XGen (IGS: XGen's spline count; Core: the generator's density as it is). `progress(i, count,
    name)`: called before each description (the window's status). Returns the ImportReport
    (report.grooms[i]["groom"] is each new groom), printed with `verbose`."""
    if not xgen_available():
        raise RuntimeError("XGen's plugin (%s) is not available" % PLUGIN)
    if isinstance(roots, str):
        roots, extra = parse_roots(roots)
        search_dirs = list(search_dirs or []) + extra
    found = xgen_descriptions()
    if descriptions is not None:
        wanted = [descriptions] if isinstance(descriptions, str) else list(descriptions)
        by_name = {d["name"]: d for d in found}
        by_node = {d["node"]: d for d in found}
        chosen = []
        for w in wanted:
            d = by_name.get(w) or by_node.get(w)
            if d is None:
                raise ValueError("no XGen description named %r (the scene has: %s)" %
                                 (w, ", ".join(sorted(by_name)) or "none"))
            chosen.append(d)
        found = chosen
    report = ImportReport()
    base = _base_resolver(roots, search_dirs)
    work = tempfile.mkdtemp(prefix="groomlab_xgen_")
    try:
        for index, d in enumerate(found):
            if progress is not None:
                progress(index, len(found), d["name"])
            report.begin(d["name"], "XGen %s" % ("Interactive Groom" if d["flavor"] == "igs" else "Core"))
            try:
                if d["flavor"] == "igs":
                    foreign = read_igs(d["node"], base, report, work, max_guides=max_guides)
                else:
                    foreign = read_core(d["palette"], d["node"], base, report)
                if foreign is None:
                    continue
                groom = build_groom(foreign, report, map_resolution=map_resolution)
                if match_count and foreign.strand_count:
                    _match_count(groom, foreign, report)
            except Exception as e:
                report.skipped("description", "import failed: %s" % e)
                if verbose:
                    import traceback
                    traceback.print_exc()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if verbose:
        print(report.text())
    return report


def _base_resolver(roots, search_dirs):
    import groomlab
    scene = groomlab.scene_folder()
    project = cmds.workspace(query=True, rootDirectory=True).replace("\\", "/")
    dirs = [d for d in [scene, project.rstrip("/")] if d] + list(search_dirs or [])
    tokens = {"PROJECT": project if project.endswith("/") else project + "/"}
    return PathResolver(tokens=tokens, roots=roots, search_dirs=dirs)


def _match_count(groom, foreign, report):
    """Scatter's density scaled so the groom grows the source's strand count (one correction)."""
    import groomlab
    from groomlab.review import groom_stats
    preview = cmds.getAttr(groom + ".previewDensity")
    cmds.setAttr(groom + ".previewDensity", 1.0)
    try:
        count = groom_stats(groom)["strands"]
        if count <= 0:
            return
        ratio = foreign.strand_count / float(count)
        if abs(ratio - 1.0) < 0.01:
            return
        graph = groomlab.get_graph(groom)
        for node in graph["nodes"]:
            if node["type"] == "Scatter":
                node.setdefault("params", {})["density"] = node["params"].get("density", foreign.density) * ratio
        groomlab.set_graph(groom, graph)
        report.approximated("density", "Scatter's density scaled by %.3f to grow XGen's %d strands (%d before)" %
                            (ratio, foreign.strand_count, count))
    finally:
        cmds.setAttr(groom + ".previewDensity", preview)


# ------------------------------------------------------------------------------------------------
# Shared readers


def _mesh_shape(node):
    if not node or not cmds.objExists(node):
        return None
    if cmds.nodeType(node) == "mesh":
        return node
    shapes = cmds.listRelatives(node, shapes=True, noIntermediate=True, fullPath=True, type="mesh") or []
    return shapes[0] if shapes else None


def _area(mesh):
    return cmds.polyEvaluate(mesh, worldArea=True) or 0.0


def _value_source(value, what, resolver, file_names, report, name_hint):
    """An XGen expression attribute as (constant, MapSource or None): a constant gives (c, None), a
    map read (scale, map), anything else (1.0, an expression MapSource)."""
    kind = seexpr.simplify(value)
    if kind[0] == "const":
        return kind[1], None
    if kind[0] == "map":
        return kind[2], _ptex_map(kind[1], what, resolver, file_names)
    text = kind[1]
    maps = [_ptex_map(p, what, resolver, file_names) for p in seexpr.map_paths(text)]
    for var in seexpr.variables(text):
        if var in seexpr.DIFFERENT_VARIABLES:
            report.approximated(what, seexpr.DIFFERENT_VARIABLES[var])
    report.mapped(what, "expression passed through: %s" % text.replace("\n", " "))
    return 1.0, MapSource("expression", expression=text, maps=maps, name=name_hint, source=what)


def _ptex_map(path, what, resolver, file_names):
    """A map() path: a folder of per-mesh Ptex files (XGen's paint maps), a .ptx, or an image."""
    clean = path.strip()
    last = os.path.basename(clean.rstrip("/\\")) or "map"
    stem, ext = os.path.splitext(last)
    if not ext:  # a folder: <folder>/<mesh>.ptx
        return MapSource("ptex", path=clean, name=last, resolver=resolver, is_dir=True, file_names=file_names,
                         source=what)
    kind = "ptex" if ext.lower() == ".ptx" else "file"
    return MapSource(kind, path=clean, name=stem, resolver=resolver, source=what)


def _shader_op(shape, report):
    """HairColor from the description's shader where it is aiStandardHair (melanin, redness, dye)."""
    groups = cmds.listConnections(shape, type="shadingEngine") or []
    for sg in groups:
        shaders = cmds.listConnections(sg + ".surfaceShader", source=True, destination=False) or []
        for shader in shaders:
            kind = cmds.nodeType(shader)
            if kind == "aiStandardHair":
                params = {"mode": "melanin",
                          "melanin": cmds.getAttr(shader + ".melanin"),
                          "redness": cmds.getAttr(shader + ".melaninRedness"),
                          "melaninRandom": cmds.getAttr(shader + ".melaninRandomize"),
                          "dye": list(cmds.getAttr(shader + ".baseColor")[0])}
                what = "%s (aiStandardHair)" % shader
                report.mapped(what, "melanin, melaninRedness, melaninRandomize, baseColor as dye")
                report.skipped(what, "the rest of the shader (GroomLab's own hair shader takes HairColor's look)")
                return ForeignOp("color", params, source=shader)
            report.skipped("shader %s (%s)" % (shader, kind), "only aiStandardHair's melanin and color are read")
    return None


# ------------------------------------------------------------------------------------------------
# XGen Core


def read_core(palette, desc, base_resolver, report):
    """A Core description as a ForeignGroom (None when it can't be: reported)."""
    import xgenm as xg

    def get(obj, attr):
        try:
            return xg.getAttr(attr, palette, desc, obj)
        except Exception:
            return None

    primitive = xg.getActive(palette, desc, "Primitive")
    generator = xg.getActive(palette, desc, "Generator")
    if primitive != "SplinePrimitive":
        report.skipped("description", "a %s description: only Spline primitives are hair" % primitive)
        return None
    geoms = list(xg.boundGeometry(palette, desc) or ())
    scalp = _mesh_shape(geoms[0]) if geoms else None
    if not scalp:
        report.skipped("description", "no bound mesh")
        return None
    if len(geoms) > 1:
        report.approximated("bound geometry", "bound to %d meshes; the groom grows on %s only" % (len(geoms), geoms[0]))
    try:
        desc_path = xg.descriptionPath(palette, desc).replace("\\", "/")
    except Exception:
        desc_path = ""
    try:
        palette_path = xg.palettePath(palette).replace("\\", "/")
    except Exception:
        palette_path = ""
    resolver = base_resolver.with_tokens(DESC=desc_path.rstrip("/"), PALETTE=palette, DESCRIPTION=desc)
    resolver.search_dirs = resolver.search_dirs + [p for p in (desc_path, palette_path) if p]
    transform = cmds.listRelatives(scalp, parent=True)[0]
    file_names = [transform + ".ptx", transform.split(":")[-1] + ".ptx", scalp.split("|")[-1] + ".ptx"]

    foreign = ForeignGroom(desc, scalp, source="XGen Core %s/%s" % (palette, desc))
    # Guides
    guides = []
    for g in xg.descriptionGuides(desc) or []:
        flat = cmds.xgmGuideGeom(guide=g, controlPoints=True) or []
        pts = [tuple(flat[i:i + 3]) for i in range(0, len(flat) - 2, 3)]
        if len(pts) >= 2:
            guides.append(pts)
    if not guides:
        report.skipped("guides", "the description has no guides (a Groomable Splines description: convert it to "
                                 "Interactive Groom in XGen, then import that)")
        return None
    foreign.guides = guides
    foreign.segments = max(8, min(32, int(_float(get("SplinePrimitive", "fxCVCount"), 5)) * 3))
    # XGen grows primitives only on the faces its guides reach: Scatter keeps to the guides' reach.
    reach = GUIDE_REACH * _guide_spacing(guides)
    foreign.scatter["guideRadius"] = reach
    report.approximated("guides", "%d guides; hair grows within %.3g of a guide root (%g x their spacing), where XGen "
                                  "grows it on the faces its guides reach" % (len(guides), reach, GUIDE_REACH))

    # Generator: density and mask
    density = _float(get(generator, "density"), 1.0)
    foreign.density = density
    report.mapped("%s.density" % generator, "Scatter density %g (strands per unit area)" % density)
    if generator == "UniformGenerator":
        report.approximated(generator, "uniform placement: Scatter's random roots (randomness 0.3)")
    mask, mask_map = _value_source(get(generator, "mask"), "%s.mask" % generator, resolver, file_names, report,
                                   "density")
    foreign.density *= mask
    foreign.density_map = mask_map
    if mask_map is not None:
        report.mapped("%s.mask" % generator, "a map on Scatter's density")
    for attr in ("cullFlag", "usePoints"):
        if str(get(generator, attr)).lower() == "true":
            report.skipped("%s.%s" % (generator, attr), "not taken over")

    prim = "SplinePrimitive"
    length, length_map = _value_source(get(prim, "length"), "SplinePrimitive.length", resolver, file_names, report,
                                       "length")
    if length_map is not None or abs(length - 1.0) > 1e-6:
        op = ForeignOp("length", {"space": "strand", "scale": length}, source="SplinePrimitive.length")
        if length_map is not None:
            op.maps["scale"] = length_map
        foreign.ops.append(op)
        report.mapped("SplinePrimitive.length", "Transform scale (it multiplies the guides' length)")
    width, width_map = _value_source(get(prim, "width"), "SplinePrimitive.width", resolver, file_names, report, "width")
    taper, _ = _value_source(get(prim, "taper"), "SplinePrimitive.taper", resolver, file_names, report, "taper")
    taper_start, _ = _value_source(get(prim, "taperStart"), "SplinePrimitive.taperStart", resolver, file_names, report,
                                   "taperStart")
    ramp = seexpr.ramp(get(prim, "widthRamp"))
    ramp_keys_ = ramp[0] if ramp else [(0.0, 1.0), (1.0, 1.0)]
    profile = []
    for i in range(11):
        t = i / 10.0
        taper_share = max(0.0, (t - taper_start) / max(1e-6, 1.0 - taper_start)) if t > taper_start else 0.0
        profile.append((t, seexpr.ramp_at(ramp_keys_, t) * (1.0 - taper * taper_share)))
    width_op = ForeignOp("width", {"width": width, "profile": ramp_keys(_thin(profile), linear=True)},
                         source="SplinePrimitive.width")
    if width_map is not None:
        width_op.maps["width"] = width_map
    foreign.ops.append(width_op)
    report.mapped("SplinePrimitive.width, taper, taperStart, widthRamp", "Width (width %g, a profile from the taper "
                                                                          "and ramp)" % width)
    for attr in ("offU", "offV", "offN", "aboutN", "depth", "bendParam[0]"):
        value = seexpr.simplify(get(prim, attr))
        default = {"bendParam[0]": 0.5, "depth": 1.0}.get(attr, 0.0)
        if value[0] != "const" or abs(value[1] - default) > 1e-6:
            report.skipped("SplinePrimitive.%s" % attr, "not taken over")

    guide_length = _median([_length(g) for g in guides]) * (length if length_map is None else 1.0)
    for module in xg.fxModules(palette, desc) or ():
        kind = xg.fxModuleType(palette, desc, module)
        what = "%s (%s)" % (module, kind)
        if str(get(module, "active")).lower() == "false":
            report.skipped(what, "inactive in XGen")
            continue
        mod_resolver = resolver.with_tokens(FXMODULE=module)
        op = None
        if kind == "ClumpingFXModule":
            op = _core_clump(module, get, mod_resolver, file_names, report, what, desc_path, len(guides))
        elif kind == "NoiseFXModule":
            op = _noise_op(get(module, "magnitude"), get(module, "frequency"), get(module, "correlation"),
                           seexpr.ramp(get(module, "magnitudeScale")), get(module, "mask"), what, mod_resolver,
                           file_names, report, guide_length)
        elif kind == "CoilFXModule":
            op = _coil_op(module, get, mod_resolver, file_names, report, what, guide_length)
        elif kind == "CutFXModule":
            op = _cut_op(get(module, "amount"), get(module, "mask"), what, mod_resolver, file_names, report)
        else:
            report.skipped(what, "no GroomLab equivalent taken over (wind, forces, wires, collisions, mesh cuts and "
                                 "animation modules are left out)")
        if op is not None:
            foreign.ops.append(op)
    shapes = cmds.listRelatives(desc, shapes=True, fullPath=True) if cmds.objExists(desc) else None
    color = _shader_op(shapes[0], report) if shapes else None
    if color:
        foreign.ops.append(color)
    return foreign


def _core_clump(module, get, resolver, file_names, report, what, desc_path, guide_count):
    strength, strength_map = _value_source(get(module, "clump"), what + " clump", resolver, file_names, report, "clump")
    params = {"strength": max(-1.0, min(1.0, strength))}
    ramp = seexpr.ramp(get(module, "clumpScale"))
    if ramp:
        params["profile"] = ramp_keys([(p, 1.0 - v) for p, v in ramp[0]], linear=ramp[1])
    # How many clumps: the different clumps in XGen's clump map, else one per guide.
    count = 0
    map_dir = (get(module, "mapDir") or "").replace("${FXMODULE}", module)
    folder, _, _ = resolver.find(map_dir, is_dir=True) if map_dir else (None, [], [])
    if folder:
        import groomlab_core
        for name in file_names:
            path = os.path.join(folder, name)
            if os.path.isfile(path):
                try:
                    count = groomlab_core.ptex_info(path)["distinct_values"]
                except Exception:
                    count = 0
                break
    inputs = {}
    if count <= 1 or abs(count - guide_count) <= max(2, 0.05 * guide_count):
        # XGen's clumps at the guides (its Setup Maps' "guides" points, or no map read): Clump's
        # centers at the guides' roots.
        params.update(centersFrom="inputPoints")
        inputs["centers"] = "guides"
        report.mapped(what + " clump points", "a clump at each guide (Clump's centers from the guides)%s" %
                      ("" if count > 1 else ": XGen's clump map was not read"))
    else:
        params.update(clumpBy="count", count=int(count))
        report.approximated(what, "clump count %d: the clumps in XGen's clump map, picked by GroomLab among the "
                                  "strands (not XGen's clump points)" % count)
    flat, _ = _value_source(get(module, "flatness"), what + " flatness", resolver, file_names, report, "flatness")
    if flat:
        params["flatness"] = max(0.0, min(1.0, flat))
    copy, _ = _value_source(get(module, "copy"), what + " copy", resolver, file_names, report, "copy")
    if copy:
        params["copyAmount"] = max(0.0, min(1.0, copy))
    if str(get(module, "clumpVolumize")).lower() == "true":
        report.skipped(what + " clumpVolumize", "Clump's volumize is the clumps' radius: set it on the Clump node")
    variance = _float(get(module, "clumpVariance"), 0.0)
    if variance:
        params["strengthRandom"] = max(0.0, min(1.0, variance))
        report.approximated(what, "clumpVariance as Clump's strengthRandom")
    op = ForeignOp("clump", params, source=what, inputs=inputs)
    if strength_map is not None:
        op.maps["strength"] = strength_map
    _mask(op, get(module, "mask"), what, resolver, file_names, report)
    for attr in ("cut", "noise", "curl", "offset", "frame"):
        v = seexpr.simplify(get(module, attr))
        if v[0] != "const" or abs(v[1]) > 1e-6:
            report.skipped("%s %s" % (what, attr), "clumping's own %s is not taken over" % attr)
    if str(get(module, "useControlMaps")).lower() in ("true", "1"):
        report.skipped(what + " control maps", "not taken over")
    report.mapped(what, "Clump (clump as strength, 1 - clumpScale as its profile)")
    return op


def _mask(op, value, what, resolver, file_names, report):
    """A module's mask on the node's affect (a constant, a map or an expression)."""
    if value is None:
        return
    c, m = _value_source(value, what + " mask", resolver, file_names, report, "mask")
    if m is not None:
        op.params["affect"] = max(0.0, min(1.0, c))
        op.maps["affect"] = m
        report.mapped(what + " mask", "the node's affect, from a map")
    elif abs(c - 1.0) > 1e-6:
        op.params["affect"] = max(0.0, min(1.0, c))
        report.mapped(what + " mask", "the node's affect %g" % c)


def _noise_op(magnitude, frequency, correlation, ramp, mask, what, resolver, file_names, report, guide_length):
    mag, mag_map = _value_source(magnitude, what + " magnitude", resolver, file_names, report, "noise")
    freq = _float(seexpr.simplify(frequency)[1] if seexpr.simplify(frequency)[0] == "const" else None, 1.0)
    # Strand space: GroomLab's frequency counts features per strand length, as XGen's does.
    params = {"amplitude": mag * NOISE_AMPLITUDE_PER_MAGNITUDE * max(guide_length, 1e-6),
              "frequency": max(0.01, freq) * NOISE_FREQUENCY_PER_FREQUENCY, "space": "strand", "octaves": 1}
    if ramp:
        params["profile"] = ramp_keys(ramp[0], linear=ramp[1])
    corr_kind = seexpr.simplify(correlation) if correlation is not None else ("const", 0.0)
    corr = corr_kind[1] if corr_kind[0] == "const" else 0.0
    if corr >= 0.5:
        params["space"] = "world"
        params["frequency"] = params["frequency"] / max(guide_length, 1e-6)  # world: per scene unit
        report.approximated(what, "correlation %g: world-space noise (coherent across strands)" % corr)
    elif corr > 0:
        report.approximated(what, "correlation %g: strand noise (each strand its own)" % corr)
    op = ForeignOp("noise", params, source=what)
    if mag_map is not None:
        op.maps["amplitude"] = mag_map
    _mask(op, mask, what, resolver, file_names, report)
    report.approximated(what, "Noise: amplitude %.4g = magnitude x %.2g x the guides' median length %.3g; frequency "
                              "%g x %g per strand length (calibrated on matching grooms)" %
                        (params["amplitude"], NOISE_AMPLITUDE_PER_MAGNITUDE, guide_length, freq,
                         NOISE_FREQUENCY_PER_FREQUENCY))
    return op


def _coil_op(module, get, resolver, file_names, report, what, guide_length):
    count, _ = _value_source(get(module, "count"), what + " count", resolver, file_names, report, "count")
    radius, radius_map = _value_source(get(module, "radius"), what + " radius", resolver, file_names, report, "radius")
    params = {"shape": "helix", "radius": radius, "frequency": count / max(guide_length, 1e-6), "randomness": 0.0,
              "phaseRandom": 1.0}
    rr = seexpr.ramp(get(module, "radiusScale"))
    if rr:
        params["profile"] = ramp_keys(rr[0], linear=rr[1])
    cr = seexpr.ramp(get(module, "countScale"))
    if cr:
        params["frequencyProfile"] = ramp_keys(cr[0], linear=cr[1])
    op = ForeignOp("curl", params, source=what, per_strand={"frequency": count})
    if radius_map is not None:
        op.maps["radius"] = radius_map
    _mask(op, get(module, "mask"), what, resolver, file_names, report)
    report.mapped(what, "Curl helix (radius, radiusScale as its profile, countScale as its frequency profile)")
    return op


def _cut_op(amount, mask, what, resolver, file_names, report):
    """XGen's Cut (an absolute length off each tip) as Length's keep: 1 - amount / length, per strand."""
    kind = seexpr.simplify(amount)
    if kind[0] == "const":
        if kind[1] <= 0:
            return None
        expr = "max(0, 1 - %g / $length)" % kind[1]
        maps = []
    elif kind[0] == "map":
        m = _ptex_map(kind[1], what + " amount", resolver, file_names)
        expr = 'max(0, 1 - %g * map("%s") / $length)' % (kind[2], kind[1])
        maps = [m]
    else:
        expr = seexpr.compose(kind[1], "max(0, 1 - ({}) / $length)")
        maps = [_ptex_map(p, what + " amount", resolver, file_names) for p in seexpr.map_paths(kind[1])]
    op = ForeignOp("cut", {"keep": 1.0}, source=what)
    op.maps["keep"] = MapSource("expression", expression=expr, maps=maps, name="cut", source=what)
    _mask(op, mask, what, resolver, file_names, report)
    report.mapped(what, "Length keep %s" % expr)
    return op


# ------------------------------------------------------------------------------------------------
# Interactive Groom Splines


def modifier_chain(shape):
    """An IGS description's nodes from its base to its last modifier."""
    chain, node = [], shape
    for _ in range(256):
        plugs = (cmds.listConnections(node + ".inSplineData", source=True, destination=False, plugs=True) or [])
        if not plugs:
            break
        node = plugs[0].split(".")[0]
        chain.append(node)
        if cmds.nodeType(node) == "xgmSplineBase":
            break
    return list(reversed(chain))


def export_splines(shape, path):
    """XGen's splines of an IGS description at the current frame into an Alembic file (XGen's own
    export), widths included. Returns the path."""
    transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
    cmds.xgmSplineCache(export=True, j=["-file %s -df ogawa -fr %d %d -step 1 -wfw -obj %s" %
                                        (path.replace("\\", "/"), cmds.currentTime(q=True), cmds.currentTime(q=True),
                                         transform)])
    return path


def read_strands(path):
    """The curves of an Alembic file as a groomlab_core StrandSet."""
    import groomlab_core
    return groomlab_core.read_alembic_curves_at(path)


def _attr_source(node, attr, what, resolver, report, work, scalp, name_hint):
    """(value, MapSource or None) of an IGS modifier's float attribute: its value, and what feeds it
    (a file texture, an xgmSeExpr expression, another texture baked at the scalp's UVs)."""
    value = cmds.getAttr("%s.%s" % (node, attr))
    plugs = cmds.listConnections("%s.%s" % (node, attr), source=True, destination=False, plugs=True) or []
    if not plugs:
        return value, None
    src, out = plugs[0].split(".", 1)
    channel = {"outColorR": "r", "outColorG": "g", "outColorB": "b", "outAlpha": "luminance"}.get(out, "luminance")
    kind = cmds.nodeType(src)
    if kind == "file":
        path = cmds.getAttr(src + ".fileTextureName") or ""
        if cmds.getAttr(src + ".uvTilingMode"):
            report.approximated(what, "%s is a UDIM set: its first tile is read" % src)
        report.mapped(what, "a map from %s" % src)
        return 1.0, MapSource("file", path=path, name=name_hint, channel=channel, resolver=resolver, source=what)
    if kind == "xgmSeExpr":
        c, m = _value_source(cmds.getAttr(src + ".expression"), what, resolver, [], report, name_hint)
        return c, m
    baked = _bake(src, scalp, work)
    if baked:
        report.approximated(what, "%s (%s) baked at the scalp's UVs" % (src, kind))
        return 1.0, MapSource("file", path=baked, name=name_hint, channel=channel, source=what)
    report.skipped(what, "fed by %s (%s), which could not be baked" % (src, kind))
    return value, None


def _bake(node, scalp, work):
    from groomlab.importers.textures import bake_texture_node
    before = set(cmds.ls(type="file") or [])
    path = bake_texture_node(node, scalp, work)
    made = set(cmds.ls(type="file") or []) - before
    if made:
        cmds.delete(list(made))
    return path


def _ramp_attr(node, attr):
    """An IGS ramp attribute (position, value, interp entries) as [(position, value)], linear?"""
    keys, linear = [], True
    for i in cmds.getAttr("%s.%s" % (node, attr), multiIndices=True) or []:
        p, v, interp = cmds.getAttr("%s.%s[%d]" % (node, attr, i))[0]
        keys.append((p, v))
        linear = linear and int(interp) == 1
    return (sorted(keys), linear) if keys else None


def _pick_guides(strands, area, target, seed=1):
    """About `target` strands spread over the scalp (each at least a spacing from the others), as
    point lists."""
    n = strands.strand_count
    roots = [strands.strand_points(i)[0] for i in range(n)]
    spacing = 0.8 * math.sqrt(max(area, 1e-9) / max(1, target))
    cell = spacing
    grid = {}
    order = list(range(n))
    random.Random(seed).shuffle(order)
    chosen = []
    for i in order:
        p = roots[i]
        key = (int(math.floor(p.x / cell)), int(math.floor(p.y / cell)), int(math.floor(p.z / cell)))
        near = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        q = roots[j]
                        if (p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2 < spacing * spacing:
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
    chosen.sort()
    return [[(q.x, q.y, q.z) for q in strands.strand_points(i)] for i in chosen]


def _width_op(strands, report, what):
    """Width from XGen's own widths: the median root width and the mean profile along the strands."""
    widths = strands.widths
    if not widths:
        return None
    roots, profile_sum, profile_n = [], [0.0] * 11, [0] * 11
    offsets = strands.offsets
    step = max(1, strands.strand_count // 2000)
    for i in range(0, strands.strand_count, step):
        b, e = offsets[i], offsets[i + 1]
        if e - b < 2 or widths[b] <= 0:
            continue
        roots.append(widths[b])
        for k in range(11):
            t = k / 10.0 * (e - b - 1)
            j = int(t)
            f = t - j
            w = widths[b + j] if j + 1 >= e - b else widths[b + j] * (1 - f) + widths[b + j + 1] * f
            profile_sum[k] += w / widths[b]
            profile_n[k] += 1
    if not roots:
        return None
    root = _median(roots)
    profile = [(k / 10.0, profile_sum[k] / max(1, profile_n[k])) for k in range(11)]
    report.mapped(what, "Width %.4g at the root (XGen's median), the mean profile along the strands" % root)
    return ForeignOp("width", {"width": root, "profile": ramp_keys(_thin(profile), linear=True)}, source=what)


def read_igs(shape, base_resolver, report, work, max_guides=None):
    """An Interactive Groom description as a ForeignGroom (None when it can't be: reported)."""
    transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
    name = transform.split("|")[-1]
    meshes = cmds.xgmSplineQuery(shape, listBoundMeshes=True) or []
    scalp = _mesh_shape(meshes[0]) if meshes else None
    if not scalp:
        report.skipped("description", "no bound mesh")
        return None
    if len(meshes) > 1:
        report.approximated("bound meshes", "bound to %d meshes; the groom grows on %s only" % (len(meshes), meshes[0]))
    chain = modifier_chain(shape)
    project = base_resolver.tokens.get("PROJECT", "")
    resolver = base_resolver.with_tokens(DESC=project + "xgen/collections/" + name)
    foreign = ForeignGroom(name, scalp, source="XGen Interactive Groom %s" % name)
    area = _area(scalp)

    # XGen's own splines: as they render (count, widths), then without the look modifiers (the guides).
    final = read_strands(export_splines(shape, os.path.join(work, name + "_final.abc")))
    foreign.strand_count = final.strand_count
    muted = {}
    try:
        for node in chain:
            if cmds.nodeType(node) in IGS_LOOK + IGS_SKIPPED and not cmds.getAttr(node + ".mute"):
                muted[node] = False
                cmds.setAttr(node + ".mute", True)
        bare = read_strands(export_splines(shape, os.path.join(work, name + "_guides.abc"))) if muted else final
    finally:
        for node in muted:
            cmds.setAttr(node + ".mute", False)
    if bare.strand_count == 0:
        report.skipped("description", "XGen made no splines")
        return None
    target = max_guides or max(32, min(2000, bare.strand_count // 25))
    foreign.guides = _pick_guides(bare, area, target)
    cv = max(2, bare.max_point_count())
    foreign.segments = max(8, min(32, (cv - 1) * 3))
    report.mapped("splines", "%d XGen splines: %d of them as guides (the look modifiers muted while reading them)" %
                  (bare.strand_count, len(foreign.guides)))
    foreign.density = final.strand_count / max(area, 1e-9)

    guide_length = _median([_length(g) for g in foreign.guides])
    for node in chain:
        kind = cmds.nodeType(node)
        what = "%s (%s)" % (node, kind)
        if kind != "xgmSplineBase" and cmds.attributeQuery("mute", node=node, exists=True) and node not in muted \
                and cmds.getAttr(node + ".mute"):
            report.skipped(what, "muted in XGen")
            continue
        if kind == "xgmSplineBase":
            mask, mask_map = _attr_source(node, "densityMask", what + " densityMask", resolver, report, work, scalp,
                                          "density")
            if mask_map is not None:
                foreign.density_map = mask_map
            report.mapped(what, "Scatter density %.4g (XGen's splines per unit area)" % foreign.density)
        elif kind == "xgmModifierClump":
            foreign.ops.append(_igs_clump(node, what, resolver, report, work, scalp, area))
        elif kind == "xgmModifierNoise":
            freq = cmds.getAttr(node + ".frequency")
            op = _noise_op(cmds.getAttr(node + ".magnitude"), freq, cmds.getAttr(node + ".correlation"),
                           _ramp_attr(node, "magnitudeScale"), None, what, resolver, [], report, guide_length)
            _igs_mask(op, node, what, resolver, report, work, scalp)
            m = _attr_source(node, "magnitude", what + " magnitude", resolver, report, work, scalp, "noise")[1]
            if m is not None:
                op.maps["amplitude"] = m
            foreign.ops.append(op)
        elif kind == "xgmModifierCut":
            op = _igs_cut(node, what, resolver, report, work, scalp)
            if op:
                foreign.ops.append(op)
        elif kind in IGS_SKIPPED:
            report.skipped(what, "linear wires are not taken over (left out of the guides too)")
        elif kind == "xgmModifierSculpt":
            layers = len(cmds.getAttr(node + ".tweaks", multiIndices=True) or [])
            report.mapped(what, "baked into the guides (%d sculpt layer%s)" % (layers, "" if layers == 1 else "s"))
        elif kind in ("xgmModifierScale", "xgmModifierGuide", "xgmModifierDisplacement", "xgmModifierCollision",
                      "xgmSplineCache", "xgmCurveToSpline"):
            report.mapped(what, "baked into the guides")
        else:
            report.skipped(what, "unknown modifier, baked into the guides as XGen computes it")
    width = _width_op(final, report, "%s width" % name)
    if width:
        foreign.ops.append(width)
    color = _shader_op(shape, report)
    if color:
        foreign.ops.append(color)
    return foreign


def _igs_mask(op, node, what, resolver, report, work, scalp):
    value, source = _attr_source(node, "mask", what + " mask", resolver, report, work, scalp, "mask")
    if source is not None:
        op.params["affect"] = max(0.0, min(1.0, value))
        op.maps["affect"] = source
    elif abs(value - 1.0) > 1e-6:
        op.params["affect"] = max(0.0, min(1.0, value))
        report.mapped(what + " mask", "the node's affect %g" % value)


def _igs_clump(node, what, resolver, report, work, scalp, area):
    strength, strength_map = _attr_source(node, "clump", what + " clump", resolver, report, work, scalp, "clump")
    params = {"strength": max(-1.0, min(1.0, strength * IGS_CLUMP_STRENGTH))}
    ramp = _ramp_attr(node, "clumpScale")
    if ramp:
        params["profile"] = ramp_keys([(p, 1.0 - v) for p, v in ramp[0]], linear=ramp[1])
    density = cmds.getAttr(node + ".pointDensity") * cmds.getAttr(node + ".pointDensityMask")
    count = max(1, int(round(density * area)))
    params.update(clumpBy="count", count=count, randomness=max(0.0, min(1.0, cmds.getAttr(node + ".pointRandomness"))),
                  seed=int(cmds.getAttr(node + ".pointSeed")))
    flat = cmds.getAttr(node + ".flatness")
    if flat:
        params["flatness"] = max(0.0, min(1.0, flat))
    copy = cmds.getAttr(node + ".copy")
    if copy:
        params["copyAmount"] = max(0.0, min(1.0, copy))
    if cmds.getAttr(node + ".clumpVolumize"):
        report.skipped(what + " clumpVolumize", "Clump's volumize is the clumps' radius: set it on the Clump node")
    variance = cmds.getAttr(node + ".clumpVariance")
    if variance:
        params["strengthRandom"] = max(0.0, min(1.0, variance))
        report.approximated(what, "clumpVariance as Clump's strengthRandom")
    op = ForeignOp("clump", params, source=what)
    if strength_map is not None:
        op.maps["strength"] = strength_map
    _igs_mask(op, node, what, resolver, report, work, scalp)
    for attr in ("cut", "noise", "curl", "offset"):
        if abs(cmds.getAttr(node + "." + attr)) > 1e-6:
            report.skipped("%s %s" % (what, attr), "the clump's own %s is not taken over" % attr)
    if cmds.getAttr(node + ".useControlMap"):
        report.skipped(what + " control map", "not taken over")
    report.mapped(what, "Clump (1 - clumpScale as its profile)")
    report.approximated(what, "%d clumps: point density %g x the scalp's area, picked by GroomLab among the strands; "
                              "strength = clump x %g (calibrated on matching grooms)" %
                        (count, density, IGS_CLUMP_STRENGTH))
    return op


def _igs_cut(node, what, resolver, report, work, scalp):
    mode = cmds.getAttr(node + ".cutMode")
    if mode == 0:
        amount, amount_map = _attr_source(node, "amount", what + " amount", resolver, report, work, scalp, "cut")
        if amount_map is not None:
            expr = 'max(0, 1 - %g * map("%s") / $length)' % (amount, amount_map.path)
            amount_map.channel = amount_map.channel or "luminance"
            source = MapSource("expression", expression=expr, maps=[amount_map], name="cut", source=what)
        elif amount <= 0:
            return None
        else:
            source = MapSource("expression", expression="max(0, 1 - %g / $length)" % amount, name="cut", source=what)
        op = ForeignOp("cut", {"keep": 1.0}, {"keep": source}, source=what)
        report.mapped(what, "Length keep %s (an absolute cut)" % source.expression)
    else:
        share = cmds.getAttr(node + ".percentage") / 100.0
        if share <= 0:
            return None
        op = ForeignOp("cut", {"keep": max(0.0, 1.0 - share)}, source=what)
        report.mapped(what, "Length keep %g (a relative cut)" % (1.0 - share))
    _igs_mask(op, node, what, resolver, report, work, scalp)
    if cmds.getAttr(node + ".minRemainLength") > 0:
        report.skipped(what + " minRemainLength", "not taken over")
    return op


# ------------------------------------------------------------------------------------------------
# Helpers


def _float(value, fallback):
    try:
        return float(value)
    except (TypeError, ValueError):
        kind = seexpr.simplify(value) if isinstance(value, str) else ("",)
        return kind[1] if kind[0] == "const" else fallback


def _guide_spacing(guides):
    """The median distance from a guide's root to the nearest other root."""
    roots = [g[0] for g in guides]
    if len(roots) < 2:
        return 1.0
    step = max(1, len(roots) // 400)
    dists = []
    for i in range(0, len(roots), step):
        p = roots[i]
        dists.append(min(math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q))) for j, q in enumerate(roots) if j != i))
    return _median(dists)


def _length(points):
    return sum(math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q))) for p, q in zip(points, points[1:]))


def _median(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return 0.0
    return values[len(values) // 2]


def _thin(points, tolerance=1e-3):
    """A linear ramp's keys without the ones on the line between their neighbours."""
    if len(points) <= 2:
        return points
    out = [points[0]]
    for a, b, c in zip(points, points[1:], points[2:]):
        t = (b[0] - out[-1][0]) / max(1e-9, c[0] - out[-1][0])
        if abs(out[-1][1] + (c[1] - out[-1][1]) * t - b[1]) > tolerance:
            out.append(b)
    out.append(points[-1])
    return out


def export_xgen_strands(description, path):
    """XGen's own strands of a description (either flavor) into an Alembic file, for comparisons:
    an IGS description's splines as XGen exports them; a Core description converted to an
    Interactive Groom by XGen (xgmGroomConvert) and its splines exported (the conversion deleted
    after). Returns the path."""
    found = {d["name"]: d for d in xgen_descriptions()}
    d = found[description]
    if d["flavor"] == "igs":
        return export_splines(d["node"], path)
    before = set(cmds.ls(type="xgmSplineDescription", long=True) or [])
    cmds.select(d["node"], replace=True)
    cmds.xgmGroomConvert(prefix="groomlabCompare_")
    made = [s for s in cmds.ls(type="xgmSplineDescription", long=True) or [] if s not in before]
    try:
        return export_splines(made[0], path)
    finally:
        for shape in made:
            for node in modifier_chain(shape):
                if cmds.objExists(node):
                    cmds.delete(node)
            if cmds.objExists(shape):
                cmds.delete(cmds.listRelatives(shape, parent=True, fullPath=True))
