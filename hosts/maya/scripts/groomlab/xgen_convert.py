# SPDX-License-Identifier: Apache-2.0
"""A groom as an XGen Interactive Groom: a one-off copy of its strands, no link back to GroomLab."""

import os

from maya import cmds

from groomlab.exports import export_alembic
from groomlab.groom import scalp_mesh
from groomlab.paths import asset_folder, asset_name


def convert_to_xgen_interactive(groom, path=None, name=None, density=1.0, render_overrides=False):
    """The groom's strands at the current frame as an XGen Interactive Groom description on its
    scalp: one spline per strand, where the strand is, groomed on from there with XGen's own tools
    (its Sculpt layer and modifiers).

    The strands are written to an Alembic file (`path`; default <the scene's exports folder>/
    <name>.abc) that an xgmCurveToSpline node reads at the base of the new description's modifier
    stack: keep that file with the scene. Nothing links back to the groom: groom on in GroomLab and
    convert again for a fresh copy. density: the share of the strands (1.0: every one, as rendered);
    render_overrides: the groom's render density, width and length scales applied, as a render
    has them. XGen roots each spline on the scalp as it is (no subdivision): a root lands where
    the strand's root is on the faceted mesh, a few hundredths of the scalp's faces off at most.

    Returns {"description": the new description's transform, "shape": its xgmSplineDescription,
    "curves": the xgmCurveToSpline node, "file": the Alembic file, "strands": splines made}."""
    from groomlab.importers import xgen as gx
    if not gx.xgen_available():
        raise RuntimeError("XGen's plugin (xgenToolkit) is not available: Interactive Groom needs it")
    scalp = scalp_mesh(groom)
    if not scalp:
        raise ValueError("%s has no scalp mesh: an Interactive Groom grows on one" % groom)
    name = name or "%s_xgi" % asset_name(groom)
    if path is None:
        path = os.path.join(asset_folder("exports"), name + ".abc")
    path = os.path.abspath(path).replace("\\", "/")
    export_alembic(groom, path, density=density, scales="none", render_overrides=render_overrides)

    selection = cmds.ls(selection=True, long=True) or []
    cmds.undoInfo(openChunk=True, chunkName="groomLab convert to XGen Interactive Groom")
    try:
        before = set(cmds.ls(type="xgmSplineDescription", long=True) or [])
        cmds.select(cmds.listRelatives(scalp, parent=True, fullPath=True)[0], replace=True)
        cmds.xgmCreateSplineDescription(name=name, createDefaultHair=False)
        made = [s for s in cmds.ls(type="xgmSplineDescription", long=True) or [] if s not in before]
        if not made:
            raise RuntimeError("XGen made no Interactive Groom description on %s" % scalp)
        shape = made[0]
        bases = cmds.ls(cmds.listHistory(shape) or [], type="xgmSplineBase") or []
        if not bases:
            raise RuntimeError("%s has no xgmSplineBase" % shape)
        base = bases[0]
        # The strands in place of the base's generated splines, under every modifier after it.
        after = cmds.listConnections(base + ".outSplineData", source=False, destination=True, plugs=True) or []
        curves = cmds.createNode("xgmCurveToSpline", name=name + "_strands")
        cmds.connectAttr(base + ".outSplineData", curves + ".inSplineData")
        cmds.connectAttr(base + ".outMeshData", curves + ".inMeshData")
        for plug in after:
            cmds.connectAttr(curves + ".outSplineData", plug, force=True)
        cmds.setAttr(curves + ".useMayaCurve", False)
        cmds.setAttr(curves + ".useAlembicCurve", True)
        cmds.setAttr(curves + ".fileName", path, type="string")
        strands = cmds.xgmSplineQuery(shape, splineCount=True)
    finally:
        if selection:
            cmds.select(selection, replace=True)
        else:
            cmds.select(clear=True)
        cmds.undoInfo(closeChunk=True)
    return {"description": cmds.listRelatives(shape, parent=True, fullPath=True)[0], "shape": shape, "curves": curves,
            "file": path, "strands": strands}
