# SPDX-License-Identifier: Apache-2.0
"""Where a scene's files live: the project's and the scene's folders, GroomLab's asset folders,
paths kept portable, files that follow a scene saved elsewhere, and the libraries shared across
scenes."""

import filecmp
import json
import os
import shutil

from maya import cmds

import groomlab_core

from groomlab.base import GROOM_TYPE
from groomlab.params import get_graph


def project_root():
    return cmds.workspace(query=True, rootDirectory=True)


def scene_folder():
    """The open scene's folder, or "" while the scene is untitled."""
    scene = cmds.file(query=True, sceneName=True) or ""
    return os.path.dirname(scene).replace("\\", "/") if scene else ""


ASSET_FOLDER = "groomlab"
ASSET_KINDS = ("masks", "cards", "exports", "caches", "renders", "references")
_warned_untitled = []


def asset_name(groom):
    """The name a groom's files take (masks, bakes, exports): its transform's full DAG path made
    file safe (every character but letters, digits, _ . and - becomes _), so two grooms of the
    same short name under different parents, or in namespaces, never share a file:
    |charA|hair -> charA_hair, charA:hair -> charA_hair."""
    return cmds.groomLabPaths(assetName=groom)


def asset_folder(kind=None, create=True):
    """Where GroomLab keeps a scene's files: <scene folder>/groomlab/<scene name>, with `kind`
    under it (masks, cards, exports, caches, renders, references), so two scenes in one folder (shot_v001,
    shot_v002) never share files. Send the scene with its groomlab folder and everything keeps
    working elsewhere: grooms store these paths relative to the scene. While the scene is
    untitled the folder is <project>/groomlab/untitled (a warning says so once). With `create`
    the folder is made. Returns the absolute path, forward slashes."""
    if not scene_folder():
        if not _warned_untitled and not cmds.about(batch=True):
            _warned_untitled.append(True)
            cmds.warning("GroomLab: the scene is untitled, so its files go under the project's %s/untitled folder; "
                         "save the scene first to keep them beside it" % ASSET_FOLDER)
            try:
                cmds.inViewMessage(amg="GroomLab: <hl>save the scene</hl> to keep its masks, bakes and exports beside "
                                       "it (they go under the project's %s/untitled folder until then)" % ASSET_FOLDER,
                                   pos="topCenter", fade=True, fadeStayTime=6000)
            except RuntimeError:
                pass
    folder = cmds.groomLabPaths(assetFolder=kind or "").replace("\\", "/")
    if create and not os.path.isdir(folder):
        os.makedirs(folder)
    return folder


_scene_files_from = [("", "")]  # (the folder the scene's relative paths resolve against, its asset folder in it)


def _scene_files_now():
    """(the folder the open scene's relative paths resolve against: its folder, or the project
    while untitled; the scene's asset folder relative to it, e.g. groomlab/shot_v001)."""
    base = (scene_folder() or project_root()).replace("\\", "/").rstrip("/")
    root = asset_folder(create=False)
    return base, os.path.relpath(root, base).replace("\\", "/")


def remember_scene_folder():
    """Note where the open scene's files live now (its folder, or the project while untitled),
    and point reference photos at their files as the scene resolves them (a scene that moved
    with its groomlab folder). The plugin calls it when a scene opens and after one saves."""
    from groomlab import reference
    _scene_files_from[0] = _scene_files_now()
    try:
        reference.relink()
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: reference photos: %s" % e)


def _same_file(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _relative(value):
    return bool(value) and not os.path.isabs(value) and "$" not in value and "%" not in value


def follow_scene_files():
    """When a save gives the scene another name or folder (Save Scene As, the first save of an
    untitled scene): the files its grooms name relative to the scene (texture maps, card bakes,
    guide caches, groom files, Simulate bakes, the images and Alembic files the graph reads) are
    copied from where they were into the new scene's own asset
    folder (groomlab/<old scene>/... -> groomlab/<new scene>/...; other relative paths keep
    their path under the new folder) and the grooms repointed there, so the saved scene is whole
    and never shares files with the scene it came from. A file already at a destination with
    other content is never adopted: the copy takes a free name (a Script Editor warning says so).
    Referenced grooms are left alone (their paths resolve against their own file). The plugin
    calls it just before every save. Returns the copied paths (relative to the new scene)."""
    from groomlab import reference
    from groomlab.cards import _BAKE_FILES, _existing_scalp_cap, _material_textures, card_mesh
    old_base, old_prefix = _scene_files_from[0]
    new_base, new_prefix = _scene_files_now()
    remember_scene_folder()
    if not scene_folder() or not old_base:
        return []
    if _same_file(old_base, new_base) and old_prefix == new_prefix:
        return []
    copied = []

    def home(rel):
        """Where a relative path goes in the new scene: its asset folder for the old one's files."""
        if old_prefix and rel.startswith(old_prefix + "/"):
            return new_prefix + rel[len(old_prefix):]
        return rel

    def conflict(src, dst):
        return os.path.exists(dst) and not _same_file(src, dst) and not filecmp.cmp(src, dst, shallow=False)

    def copy(src, dst, rel):
        if _same_file(src, dst) or os.path.exists(dst):
            return  # the same file, or one with the same content
        folder = os.path.dirname(dst)
        if folder and not os.path.isdir(folder):
            os.makedirs(folder)
        shutil.copy2(src, dst)
        copied.append(rel)

    def source(rel):
        """The folder a relative path was found in: the old scene's, else the project (as it resolves)."""
        for base in (old_base, project_root()):
            if base and os.path.isfile(os.path.join(base, rel)):
                return base
        return None

    def follow(rel):
        """A single file: its new stored path (None: nothing to copy, the path stays)."""
        if not source(rel):
            return None
        src = os.path.join(source(rel), rel)
        new_rel = home(rel)
        dst = os.path.join(new_base, new_rel)
        if conflict(src, dst):
            dst = _unique_path(dst)
            new_rel = os.path.relpath(dst, new_base).replace("\\", "/")
            cmds.warning("GroomLab: %s is already beside the saved scene with other content; this scene now uses "
                         "a copy, %s" % (rel, new_rel))
        copy(src, dst, new_rel)
        return new_rel

    for groom in cmds.ls(type=GROOM_TYPE, long=True) or []:
        if cmds.referenceQuery(groom, isNodeReferenced=True):
            continue
        # Texture maps (scalpMaps)
        text = cmds.getAttr(groom + ".scalpMaps") or ""
        data = json.loads(text) if text.strip() else {}
        touched = False
        for entry in data.values():
            value = (entry.get("texture") or "").replace("\\", "/") if isinstance(entry, dict) else ""
            if entry.get("kind") == "texture" and _relative(value):
                new = follow(value)
                if new and new != value:
                    entry["texture"] = new
                    touched = True
        if touched:
            cmds.setAttr(groom + ".scalpMaps", json.dumps(data), type="string")
        # The card bake: every file of its prefix moves together
        prefix = groomlab_core.card_maps_prefix(cmds.getAttr(groom + ".cardMaps") or "").replace("\\", "/")
        if prefix and _relative(prefix + "Color.tga"):
            old_root = source(prefix + "Color.tga") or source(prefix + "Atlas.json") or old_base
            files = [f for f in _BAKE_FILES if os.path.isfile(os.path.join(old_root, prefix + f))]
            if files:
                new_prefix_rel = home(prefix)
                if any(conflict(os.path.join(old_root, prefix + f), os.path.join(new_base, new_prefix_rel + f))
                       for f in files):
                    stem, n = new_prefix_rel.rstrip("_"), 2
                    while any(os.path.exists(os.path.join(new_base, "%s_%d_%s" % (stem, n, f))) for f in _BAKE_FILES):
                        n += 1
                    new_prefix_rel = "%s_%d_" % (stem, n)
                    cmds.warning("GroomLab: %s's card bake is already beside the saved scene with other content; this "
                                 "scene now uses a copy, %s*" % (groom, new_prefix_rel))
                for f in files:
                    copy(os.path.join(old_root, prefix + f), os.path.join(new_base, new_prefix_rel + f), new_prefix_rel + f)
                if new_prefix_rel != prefix:
                    cmds.setAttr(groom + ".cardMaps", groomlab_core.with_card_maps_prefix(
                        cmds.getAttr(groom + ".cardMaps") or "", new_prefix_rel), type="string")
        for attr in ("guideCache", "groomFile"):
            value = (cmds.getAttr(groom + "." + attr) or "").replace("\\", "/")
            if _relative(value):
                new = follow(value)
                if new and new != value:
                    cmds.setAttr(groom + "." + attr, new, type="string")
        # The Simulate nodes' bakes.
        text = cmds.getAttr(groom + ".simCaches") or ""
        bakes = json.loads(text) if text.strip() else {}
        touched = False
        for bake in bakes.values() if isinstance(bakes, dict) else []:
            value = (bake.get("file") or "").replace("\\", "/") if isinstance(bake, dict) else ""
            if _relative(value):
                new = follow(value)
                if new and new != value:
                    bake["file"] = new
                    touched = True
        if touched:
            cmds.setAttr(groom + ".simCaches", json.dumps(bakes), type="string")
        # The files the graph reads (images, Alembic files, texture maps in params; inside
        # compounds too). A UDIM set's tiles go together.
        from groomlab import presets
        graph = get_graph(groom)
        touched = False
        for _label, params, name, path in list(presets._file_params(graph)):
            value = path.replace("\\", "/")
            if not _relative(value):
                continue
            token = next((t for t in presets.UDIM_TOKENS if t in value), None)
            if token:  # a UDIM set: its tiles follow together, the token kept
                import glob
                folder, base = os.path.split(value)
                root = next((b for b in (old_base, project_root()) if b and os.path.isdir(os.path.join(b, folder))), None)
                pattern = glob.escape(base).replace(glob.escape(token), "[1-9][0-9][0-9][0-9]")
                tiles = sorted(glob.glob(os.path.join(glob.escape(os.path.join(root, folder)), pattern))) if root else []
                for tile in tiles:
                    follow(folder + "/" + os.path.basename(tile))
                new = home(value) if tiles else None
            else:
                new = follow(value)
            if new and new != value:
                presets.set_file_param(params, name, new)
                touched = True
        if touched:
            from groomlab.params import set_graph
            set_graph(groom, graph)
        # The card mesh's and hair cap's materials (absolute paths into the old scene's folder).
        for texture in _material_textures(card_mesh(groom)) + _material_textures(_existing_scalp_cap(groom)):
            value = cmds.getAttr(texture + ".fileTextureName") or ""
            rel = _relative_to(value, old_base) if value else None
            if rel is not None and home(rel) != rel:
                new = follow(rel)
                if new:
                    cmds.setAttr(texture + ".fileTextureName", new_base + "/" + new, type="string")
    # Reference photos (Groom Tools > Photos) travel the same way.
    for camera in reference.list_references():
        if cmds.referenceQuery(camera, isNodeReferenced=True):
            continue
        value = (cmds.getAttr(camera + ".glRefImage") or "").replace("\\", "/")
        if _relative(value):
            new = follow(value)
            if new and new != value:
                reference._set_image(camera, new, new_base + "/" + new)
    cmds.groomLabPaths()  # the resolver follows the new scene folder
    if copied:
        for groom in cmds.ls(type=GROOM_TYPE) or []:
            if not cmds.referenceQuery(groom, isNodeReferenced=True):
                _bump(groom)
        if not cmds.about(batch=True):
            try:
                cmds.inViewMessage(amg="GroomLab: %d file(s) copied into the saved scene's folder (%s)"
                                       % (len(copied), new_base + "/" + new_prefix), pos="topCenter", fade=True)
            except RuntimeError:
                pass
    return copied


def _bump(groom):
    """Make the groom evaluate again (a map's file moved or rewritten)."""
    cmds.setAttr(groom + ".brushTick", (cmds.getAttr(groom + ".brushTick") or 0) + 1)


def portable_path(path):
    """`path` relative to the scene's folder, else to the Maya project, when it is inside one
    (forward slashes), else unchanged: every host's rule for a scene (the core's portablePath,
    paths.h; a groom file's paths may also lead beside its folder with "..").

    GroomLab resolves relative paths against the scene's folder, then the project root, so
    grooms keep working when the scene and its groomlab folder move together, when the project
    moves, and when referenced. Paths with environment variables ($VAR) are kept as they are.
    """
    if not path:
        return path
    return groomlab_core.portable_path(path, [f for f in (scene_folder(), project_root()) if f], False)


def _relative_to(path, base):
    """`path` relative to the folder `base` when inside it (forward slashes), else None. Folders
    compare as the file system sees them (case, and Windows' short 8.3 names)."""
    if not base:
        return None
    full, root = os.path.abspath(path), os.path.abspath(base)
    if os.path.normcase(full).startswith(os.path.normcase(root).rstrip("\\/") + os.sep):
        return os.path.relpath(full, root).replace("\\", "/")
    if not os.path.isdir(root):
        return None
    parts, head = [], full
    while True:
        parent, tail = os.path.split(head)
        if not tail or parent == head:
            return None
        parts.append(tail)
        head = parent
        if os.path.isdir(head) and os.path.samefile(head, root):
            return "/".join(reversed(parts))


def resolve_path(path, node=None):
    """A stored path as GroomLab resolves it (variables expanded, relative to the scene's folder,
    then the project). With `node`, as that node resolves it: a node from a referenced file looks
    in that file's folder first."""
    if node:
        return cmds.groomLabPaths(resolve=path, node=node)
    return cmds.groomLabPaths(resolve=path)


def _unique_path(path):
    """`path` when nothing is there, else the first free <stem>_2<ext>, <stem>_3<ext>..."""
    if not os.path.exists(path):
        return path.replace("\\", "/")
    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists("%s_%d%s" % (stem, n, ext)):
        n += 1
    return ("%s_%d%s" % (stem, n, ext)).replace("\\", "/")


def absolute_paths(groom):
    """Absolute file paths stored on a groom (maps, ImportCurves, caches): what breaks when a project moves."""
    from groomlab import presets
    found = []
    graph = get_graph(groom)
    for node in graph.get("nodes", []):
        for key, value in node.get("params", {}).items():
            path = value.get("texture") if isinstance(value, dict) else (value if key in presets.FILE_PARAM_NAMES else None)
            if isinstance(path, str) and os.path.isabs(path):
                found.append(("%s.%s" % (node["name"], key), path))
    for attr in ("guideCache", "groomFile"):
        value = cmds.getAttr(groom + "." + attr) or ""
        if os.path.isabs(value):
            found.append((attr, value))
    return found


# ---------------------------------------------------------------------------
# Libraries shared across scenes (presets, expressions, themes): JSON files looked up in a
# studio's folders, then the user's, then the ones shipped with GroomLab


def library_user_dir(env, folder):
    """A library's user folder, where GroomLab saves to it: $`env` when set, else
    <Maya user app dir>/groomlab/<folder>."""
    override = os.environ.get(env)
    if override:
        return override
    return os.path.join(cmds.internalVar(userAppDir=True), "groomlab", folder)


def library_dirs(path_env, user_dir, shipped):
    """Where a library is looked up, in order: every directory on $`path_env` (os.pathsep-
    separated), the user's folder (`user_dir()`; left out when Maya cannot say, as in a mayapy
    not initialized) and the `shipped` folder. An entry in an earlier one hides a later one."""
    dirs = [d for d in os.environ.get(path_env, "").split(os.pathsep) if d]
    try:
        dirs.append(user_dir())
    except (ImportError, RuntimeError, AttributeError):  # AttributeError: maya.cmds before initialize()
        pass
    dirs.append(shipped)
    return dirs
