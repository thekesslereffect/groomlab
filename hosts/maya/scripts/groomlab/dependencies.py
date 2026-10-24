# SPDX-License-Identifier: Apache-2.0
"""Every file a groom uses, for pipelines and Maya's File Path Editor.

file_dependencies(groom) lists them: its texture maps, the file params of its graph (inside
compounds too), its guide cache, its Simulate nodes' bakes, its card bake and the groom file Arnold
renders. repath_file points one of them elsewhere, kept portable as GroomLab keeps its paths
(relative to the scene, else the project). A pipeline (a farm submitter, a publish or archive
script) collects a scene's GroomLab files with file_dependencies over cmds.ls(type="groomLabShape"),
or with `ls -type groomLabFile` and each node's fileName.

Maya's File Path Editor lists one file attribute per node, so each file has a groomLabFile node of
its own (file_node.h: fileName as it resolves, connected to its groom, fileKind and fileKey saying
which file it is), listed under GroomLab. sync keeps a groom's nodes in step with its files: on
idle after the groom's files change (the shape asks: sync_soon) and before every save (sync_all).
A path set on a node from outside (the editor's Repath or Replace String) goes back to the groom on
idle (file_node_changed). Neither the nodes' upkeep nor a repath through them makes an undo step
of its own: undoing the editor's change of a node brings the groom's path back with it.
Referenced grooms keep the nodes their file saved."""

import json
import os
import re

from maya import cmds

import groomlab_core

from groomlab.base import GROOM_TYPE
from groomlab.params import get_graph, set_graph
from groomlab.paths import portable_path, resolve_path

FILE_TYPE = "groomLabFile"
KINDS = ("map", "param", "attribute", "simCache", "cardBake")


def file_dependencies(groom):
    """The files a groom uses: [{"kind", "key", "path", "resolved", "exists"}], `path` as stored,
    `resolved` as the groom finds it (absolute), `exists` whether it is there. Kinds: "map" (key:
    the texture map's name), "param" (node.param, compound/inner.param inside compounds),
    "attribute" (guideCache, groomFile), "simCache" (the Simulate node), "cardBake" (the bake file's
    suffix, Color.tga...: the files of one bake share a prefix)."""
    from groomlab import presets
    from groomlab.cards import _BAKE_FILES
    out = []

    def add(kind, key, path):
        if not path:
            return
        resolved = (resolve_path(path, groom) or path).replace("\\", "/")
        out.append({"kind": kind, "key": key, "path": path, "resolved": resolved,
                    "exists": presets.file_exists(resolved)})

    # (Read from the attribute: the shape's own list waits for a commit in flight, and the
    # File Path Editor's upkeep runs after every edit.)
    text = cmds.getAttr(groom + ".scalpMaps") or ""
    stored = json.loads(text) if text.strip() else {}
    for name, entry in sorted(stored.items() if isinstance(stored, dict) else []):
        if isinstance(entry, dict) and entry.get("kind") == "texture":
            add("map", name, entry.get("texture") or "")
    for label, _params, _name, path in presets._file_params(get_graph(groom)):
        add("param", label, path)
    for attr in ("guideCache", "groomFile"):
        add("attribute", attr, cmds.getAttr(groom + "." + attr) or "")
    for node, bake in sorted(_sim_caches(groom).items()):
        add("simCache", node, bake.get("file") if isinstance(bake, dict) else "")
    prefix = groomlab_core.card_maps_prefix(cmds.getAttr(groom + ".cardMaps") or "")
    if prefix:
        for suffix in _BAKE_FILES:
            if os.path.isfile(resolve_path(prefix + suffix, groom) or ""):
                add("cardBake", suffix, prefix + suffix)
    return out


def _sim_caches(groom):
    text = cmds.getAttr(groom + ".simCaches") or ""
    data = json.loads(text) if text.strip() else {}
    return data if isinstance(data, dict) else {}


def repath_file(groom, kind, key, path):
    """Point one of the groom's files (file_dependencies' kind and key) at `path`, stored portable.
    A card bake moves as a whole: the path of one of its files gives the bake's new prefix (the
    others are looked for beside it). Returns the stored path. Raises ValueError for a file the
    groom does not have or a path that does not fit."""
    stored = portable_path(path.replace("\\", "/"))
    if kind == "map":
        cmds.groomLabScalpMaps(groom, relink=key, texture=stored)
    elif kind == "param":
        from groomlab import presets
        graph = get_graph(groom)
        for label, params, name, _old in presets._file_params(graph):
            if label == key:
                presets.set_file_param(params, name, stored)
                set_graph(groom, graph)
                break
        else:
            raise ValueError("%s has no file param %s" % (groom, key))
    elif kind == "attribute":
        if key not in ("guideCache", "groomFile"):
            raise ValueError("%s is not a file attribute" % key)
        cmds.setAttr(groom + "." + key, stored, type="string")
    elif kind == "simCache":
        bakes = _sim_caches(groom)
        if key not in bakes:
            raise ValueError("%s has no bake of %s" % (groom, key))
        bakes[key]["file"] = stored
        cmds.setAttr(groom + ".simCaches", json.dumps(bakes), type="string")
    elif kind == "cardBake":
        if not stored.endswith(key):
            raise ValueError("a card bake's %s keeps its name (%s)" % (key, path))
        cmds.setAttr(groom + ".cardMaps", groomlab_core.with_card_maps_prefix(
            cmds.getAttr(groom + ".cardMaps") or "", stored[:-len(key)]), type="string")
    else:
        raise ValueError("no file kind %r" % kind)
    return stored


# ---- The groomLabFile nodes (the File Path Editor's) -----------------------------------------------

def file_nodes(groom):
    """The groom's groomLabFile nodes, by (kind, key)."""
    out = {}
    for node in cmds.listConnections(groom + ".message", destination=True, source=False, type=FILE_TYPE) or []:
        out[(cmds.getAttr(node + ".fileKind"), cmds.getAttr(node + ".fileKey"))] = node
    return out


def _same(a, b):
    return os.path.normcase(os.path.normpath(a or "")) == os.path.normcase(os.path.normpath(b or ""))


def _node_name(groom, kind, key):
    transform = (cmds.listRelatives(groom, parent=True) or [groom])[0].split("|")[-1]
    return "%s_%s_%s" % (transform, kind, re.sub(r"[^0-9A-Za-z_]", "_", key))


def sync(groom):
    """The groom's groomLabFile nodes as its files are now: one each (made, renamed as needed),
    their fileName the file as it resolves; nodes of files it no longer has removed. No undo step.
    A referenced groom is left as its file has it."""
    if not cmds.objExists(groom) or cmds.referenceQuery(groom, isNodeReferenced=True):
        return
    have = file_nodes(groom)
    wanted = {(f["kind"], f["key"]): f["resolved"] for f in file_dependencies(groom)}
    state = cmds.undoInfo(query=True, stateWithoutFlush=True)
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        for key, node in have.items():
            if key not in wanted:
                cmds.delete(node)
        for (kind, key), resolved in sorted(wanted.items()):
            node = have.get((kind, key))
            if node is None:
                node = cmds.createNode(FILE_TYPE, name=_node_name(groom, kind, key), skipSelect=True)
                cmds.connectAttr(groom + ".message", node + ".groom")
                cmds.setAttr(node + ".fileKind", kind, type="string")
                cmds.setAttr(node + ".fileKey", key, type="string")
            if not _same(cmds.getAttr(node + ".fileName"), resolved):
                cmds.setAttr(node + ".fileName", resolved, type="string")
    finally:
        cmds.undoInfo(stateWithoutFlush=state)


def sync_all():
    """Every groom's nodes in step, and nodes whose groom is gone removed (before a save)."""
    for groom in cmds.ls(type=GROOM_TYPE, long=True) or []:
        sync(groom)
    remove_orphans()


def remove_orphans():
    """The groomLabFile nodes whose groom is gone (deleted, or its making undone), removed."""
    orphans = [n for n in cmds.ls(type=FILE_TYPE) or []
               if not cmds.listConnections(n + ".groom", source=True, destination=False)
               and not cmds.referenceQuery(n, isNodeReferenced=True)]
    if orphans:
        state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            cmds.delete(orphans)
        finally:
            cmds.undoInfo(stateWithoutFlush=state)


_pending = set()


def sync_soon(groom):
    """sync, once for every request in a row (the shape asks on idle after its files change)."""
    if groom in _pending:
        return
    _pending.add(groom)
    import maya.utils

    def run():
        _pending.discard(groom)
        try:
            sync(groom)
            remove_orphans()
        except (RuntimeError, ValueError) as e:
            cmds.warning("GroomLab: %s's files for the File Path Editor: %s" % (groom, e))

    maya.utils.executeDeferred(run)


def file_node_changed(node):
    """A groomLabFile node's fileName set from outside (the editor's Repath): its groom takes the
    path when it differs from what the groom has (the node's own upkeep comes back as no change).
    No undo step of its own: the node's change is the step."""
    if not cmds.objExists(node):
        return
    grooms = cmds.listConnections(node + ".groom", source=True, destination=False, shapes=True) or []
    if not grooms:
        return
    groom = grooms[0]
    kind, key, path = cmds.getAttr(node + ".fileKind"), cmds.getAttr(node + ".fileKey"), cmds.getAttr(node + ".fileName") or ""
    current = next((f for f in file_dependencies(groom) if (f["kind"], f["key"]) == (kind, key)), None)
    if current is None or not path or _same(current["resolved"], path):
        return
    state = cmds.undoInfo(query=True, stateWithoutFlush=True)
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        repath_file(groom, kind, key, path)
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s: %s (the groom keeps %s)" % (node, e, current["path"]))
        sync(groom)  # the node shows the groom's path again
    finally:
        cmds.undoInfo(stateWithoutFlush=state)


# ---- Collect Files ---------------------------------------------------------------------------------

# Where Collect Files puts each kind of file in the scene's asset folder (paths.asset_folder).
_COLLECT_FOLDERS = {"map": "masks", "param": "masks", "attribute": "caches", "simCache": "caches", "cardBake": "cards"}


def _inside(path, folder):
    full, root = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(folder))
    return full.startswith(root.rstrip("\\/") + os.sep)


def _tiles(path):
    """The files a path names: itself, or a UDIM set's tiles."""
    from groomlab import presets
    import glob
    for token in presets.UDIM_TOKENS:
        if token in path:
            folder, base = os.path.split(path)
            pattern = glob.escape(base).replace(glob.escape(token), "[1-9][0-9][0-9][0-9]")
            return sorted(glob.glob(os.path.join(glob.escape(folder), pattern)))
    return [path] if os.path.isfile(path) else []


def _copy_into(src, folder):
    """`src` copied into `folder` under its own name (the same content there already: that file; other
    content: a free name). Returns the copy's path."""
    import filecmp
    import shutil
    from groomlab.paths import _unique_path
    if not os.path.isdir(folder):
        os.makedirs(folder)
    dst = os.path.join(folder, os.path.basename(src)).replace("\\", "/")
    if os.path.exists(dst) and not filecmp.cmp(src, dst, shallow=False):
        dst = _unique_path(dst)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    return dst


def collect_files(grooms=None):
    """Copy every file the grooms use (file_dependencies) that lies outside the scene's folder into
    its asset folder (groomlab/<scene>/masks/<groom>, caches/<groom>, cards/<groom>...) and point the
    grooms at the copies, so the scene's folder holds all of it (Archive Scene, a zip, a copy to
    another machine). A file there already stays; a missing file is reported, not looked for (Find
    Missing Files does that). A UDIM set copies every tile, a card bake every file of it. `grooms`:
    a groom or a list (default every groom). One undo step (the copies stay). Returns {"copied":
    {groom: {kind: {key: stored path}}}, "missing": {groom: {kind: {key: path}}}, "inside": files
    already in the scene's folder}. Raises ValueError while the scene is untitled."""
    from groomlab.base import undo_chunk
    from groomlab.paths import asset_folder, asset_name, scene_folder
    if not scene_folder():
        raise ValueError("save the scene first: its files are collected into its folder")
    if grooms is None:
        grooms = cmds.ls(type=GROOM_TYPE, long=True) or []
    elif isinstance(grooms, str):
        grooms = [grooms]
    report = {"copied": {}, "missing": {}, "inside": 0}

    def note(section, groom, kind, key, value):
        report[section].setdefault(groom, {}).setdefault(kind, {})[key] = value

    with undo_chunk("collect files"):
        for groom in grooms:
            if cmds.referenceQuery(groom, isNodeReferenced=True):
                continue  # a referenced groom's files belong to its file
            bake_done = False
            for f in file_dependencies(groom):
                kind, key, resolved = f["kind"], f["key"], f["resolved"]
                if _inside(resolved, scene_folder()):
                    report["inside"] += 1
                    continue
                files = _tiles(resolved)
                if not files:
                    note("missing", groom, kind, key, f["path"])
                    continue
                if kind == "cardBake":  # the bake moves as a whole: every file of its prefix
                    if bake_done:
                        continue
                    bake_done = True
                    prefix = resolved[:-len(key)]
                    from groomlab.cards import _BAKE_FILES
                    target = "%s/%s" % (asset_folder("cards"), asset_name(groom))
                    copies = [_copy_into(prefix + s, target) for s in _BAKE_FILES if os.path.isfile(prefix + s)]
                    moved = next(c for c in copies if c.endswith(key))
                    note("copied", groom, kind, key, repath_file(groom, kind, key, moved))
                    continue
                folder = _COLLECT_FOLDERS.get(kind, "caches")
                if kind == "attribute" and key == "groomFile":
                    folder = "exports"
                target = "%s/%s" % (asset_folder(folder), asset_name(groom))
                copies = [_copy_into(t, target) for t in files]
                # (A UDIM set: its token kept, in the new folder.)
                stored = target + "/" + os.path.basename(resolved) if len(files) > 1 else copies[0]
                note("copied", groom, kind, key, repath_file(groom, kind, key, stored))
    return report
