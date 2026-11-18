# SPDX-License-Identifier: Apache-2.0
"""Finding the files a foreign groom names (XGen's maps, Yeti's textures), once, at import.

Pure Python (no Maya): a path as the source wrote it is expanded and looked for, and either
found or reported with every place it was looked for. Nothing keeps the source paths afterwards:
the importer copies what it finds into GroomLab's masks folder.

    resolver = PathResolver(tokens={"DESC": "C:/proj/xgen/collections/hair/desc"},
                            roots={"ASSETS": "D:/library"}, search_dirs=["C:/proj/scenes"])
    found, tried, unset = resolver.find("${ASSETS}/maps/density.png")

Variables: ${NAME}, $NAME and %NAME%, looked up in `tokens` (what the source defines: XGen's
${DESC}, ${PROJECT}, ${FXMODULE}...), then `roots` (what the user points a variable at, the
import dialog's Roots field), then the environment. A leading ~ is the home folder. A relative
path is tried against each of `search_dirs` in turn (the scene's folder, the project, the source
file's folder...). A variable set nowhere leaves the path unresolved: `unset` names it, so the
report can say which root to give.
"""

import os
import re

_VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)|%([A-Za-z_][A-Za-z0-9_]*)%")


def parse_roots(text):
    """The import dialog's Roots field as (roots, search_dirs): one entry per line (or separated
    by ";"), NAME=folder pointing a variable at a folder, a bare folder searched for files by
    their relative path and their name."""
    roots, dirs = {}, []
    for entry in re.split(r"[;\n]", text or ""):
        entry = entry.strip()
        if not entry:
            continue
        name, sep, folder = entry.partition("=")
        name = name.strip().lstrip("$").strip("{}%")
        if sep and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name) and not re.match(r"^[A-Za-z]$", name):
            roots[name] = folder.strip()
        else:
            dirs.append(entry)
    return roots, dirs


class PathResolver(object):
    def __init__(self, tokens=None, roots=None, search_dirs=None, environ=None):
        self.tokens = dict(tokens or {})
        self.roots = dict(roots or {})
        self.search_dirs = [d for d in (search_dirs or []) if d]
        self.environ = os.environ if environ is None else environ

    def with_tokens(self, **tokens):
        """A copy with more tokens (a description's ${DESC}, a module's ${FXMODULE})."""
        merged = dict(self.tokens)
        merged.update(tokens)
        return PathResolver(merged, self.roots, self.search_dirs, self.environ)

    def lookup(self, name):
        """A variable's value, or None: tokens, then roots, then the environment."""
        for table in (self.tokens, self.roots):
            if name in table and table[name] is not None:
                return table[name]
        return self.environ.get(name)

    def expand(self, path):
        """(the path with every variable it can expand expanded, the names it could not)."""
        unset = []

        def sub(match):
            name = match.group(1) or match.group(2) or match.group(3)
            value = self.lookup(name)
            if value is None:
                if name not in unset:
                    unset.append(name)
                return match.group(0)
            return str(value)

        text = path or ""
        for _ in range(8):  # a value may name another variable
            expanded = _VARIABLE.sub(sub, text)
            if expanded == text:
                break
            text = expanded
        if text.startswith("~"):
            text = os.path.expanduser(text)
        return text.replace("\\", "/"), unset

    def candidates(self, path):
        """(the paths to try for `path`, in order, the variables left unset)."""
        expanded, unset = self.expand(path)
        if unset:
            return [], unset
        out = []
        if os.path.isabs(expanded):
            out.append(expanded)
        else:
            out.extend(_join(d, expanded) for d in self.search_dirs)
        return _unique(out), unset

    def find(self, path, is_dir=False):
        """(the file, or folder with `is_dir`, `path` names, or None; every place tried; the
        variables left unset). A file not where it is named is then looked for by name directly
        under each search folder (a texture sent beside the scene)."""
        candidates, unset = self.candidates(path)
        tried = []
        exists = os.path.isdir if is_dir else os.path.isfile
        for c in candidates:
            tried.append(c)
            if exists(c):
                return os.path.normpath(c).replace("\\", "/"), tried, unset
        if not is_dir:
            name = os.path.basename((self.expand(path)[0] or "").rstrip("/"))
            if name:
                for d in self.search_dirs:
                    c = _join(d, name)
                    if c in tried:
                        continue
                    tried.append(c)
                    if os.path.isfile(c):
                        return os.path.normpath(c).replace("\\", "/"), tried, unset
        return None, tried, unset


def _join(folder, path):
    return os.path.normpath(os.path.join(folder, path)).replace("\\", "/")


def _unique(paths):
    seen, out = set(), []
    for p in paths:
        key = os.path.normcase(p)
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out
