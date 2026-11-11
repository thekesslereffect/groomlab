# SPDX-License-Identifier: GPL-3.0-or-later
"""A client of groomlab-helper (docs/BLENDER.md): spawn it, send one JSON line per request, read one
JSON line per response, read arrays from shared memory with numpy (no per-point Python loops) and
hand big inputs over in a block of its own.

    with HelperClient(exe) as h:
        s = h.call("new_session", space={"upAxis": "Z", "metersPerUnit": 1.0})["session"]
        h.call("set_scalp", session=s, **h.put(points=pts, faceCounts=counts, faceIndices=idx))
        r = h.call("evaluate", session=s)
        positions = h.array(r["arrays"]["positions"])        # (points, 3) float32, a copy

Plain Python 3.8+ with numpy, no bpy: the add-on uses it, and so do the helper's tests
(tests/hosts/blender/helper_client.py loads this file).
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
from multiprocessing import shared_memory

import numpy as np

CREATE_NO_WINDOW = 0x08000000


def sentence(text):
    """A message as Blender's reports show it: a capital first letter."""
    text = str(text or "")
    return text[:1].upper() + text[1:]


class HelperError(RuntimeError):
    def __init__(self, code, message):
        RuntimeError.__init__(self, "%s: %s" % (code, message))
        self.code = code
        self.message = message


def cancelled(op, e):
    """An operator's failure: `e` (a HelperError's message, else the exception's text) reported as an
    error, {"CANCELLED"} returned. Every operator module's _fail."""
    op.report({"ERROR"}, sentence(e.message if isinstance(e, HelperError) else str(e)))
    return {"CANCELLED"}


def _open_block(name):
    if sys.platform == "win32" or sys.version_info < (3, 13):
        return shared_memory.SharedMemory(name=name)
    return shared_memory.SharedMemory(name=name, track=False)  # the helper unlinks its own


class HelperClient(object):
    """One helper process. `log`: a path for the helper's stderr (its log), else a temporary file.
    Requests are serialized (a lock), so a render thread and the UI may share one client."""

    def __init__(self, exe, env=None, args=(), log=None):
        self.log = open(log, "w+b") if log else tempfile.TemporaryFile()
        flags = CREATE_NO_WINDOW if sys.platform == "win32" else 0
        self.proc = subprocess.Popen([exe, "--parent-pid", str(os.getpid())] + list(args), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, env=env, creationflags=flags)
        self.lock = threading.RLock()
        self._next = 1
        self._blocks = {}   # the helper's blocks we opened, by name
        self._inputs = []   # our blocks for the request in flight

    def alive(self):
        return self.proc.poll() is None

    # ---- Requests ---------------------------------------------------------------------------------

    def send_line(self, text):
        """Writes a raw line and returns the parsed response line (tests: malformed requests)."""
        with self.lock:
            try:
                self.proc.stdin.write(text.encode("utf-8") + b"\n")
                self.proc.stdin.flush()
                line = self.proc.stdout.readline()
            except (OSError, ValueError):
                line = b""
            if not line:
                raise HelperError("gone", "the helper ended (exit code %s): %s" % (self.proc.poll(), self.stderr()[-2000:]))
            return json.loads(line.decode("utf-8"))

    def request(self, cmd, **args):
        """The whole response: {"id", "ok", "result" | "error"}."""
        with self.lock:
            rid = self._next
            self._next += 1
            message = dict(args)
            message["id"] = rid
            message["cmd"] = cmd
            try:
                response = self.send_line(json.dumps(message))
            finally:
                self._release_inputs()
            if response.get("id") != rid:
                raise HelperError("protocol", "answer %r to request %d" % (response.get("id"), rid))
            return response

    def call(self, cmd, **args):
        """The result, or HelperError."""
        response = self.request(cmd, **args)
        if not response["ok"]:
            raise HelperError(response["error"]["code"], response["error"]["message"])
        return response["result"]

    # ---- Shared memory ----------------------------------------------------------------------------

    def view(self, ref):
        """An array the helper answered with, as a numpy view of its block (valid until the next
        answer of the same kind)."""
        name = ref["shm"]
        block = self._blocks.get(name)
        if block is None:
            block = self._blocks[name] = _open_block(name)
        return np.ndarray(tuple(ref["shape"]), dtype=np.dtype(ref["dtype"]), buffer=block.buf, offset=ref["offset"])

    def array(self, ref):
        """The same, copied out."""
        return np.array(self.view(ref), copy=True)

    def put(self, **arrays):
        """Arguments for a request: each array in one new block of ours, as references. The block
        lives until the response came."""
        prepared = []
        size = 0
        for name, value in arrays.items():
            a = np.ascontiguousarray(value)
            if a.dtype == np.float64:
                a = a.astype(np.float32)
            offset = (size + 63) & ~63
            prepared.append((name, a, offset))
            size = offset + a.nbytes
        block = shared_memory.SharedMemory(create=True, size=max(size, 1))
        with self.lock:
            self._inputs.append(block)
        out = {}
        for name, a, offset in prepared:
            np.ndarray(a.shape, dtype=a.dtype, buffer=block.buf, offset=offset)[...] = a
            out[name] = {"shm": block.name, "offset": offset, "dtype": a.dtype.str, "shape": list(a.shape)}
        return out

    def _release_inputs(self):
        for block in self._inputs:
            try:
                block.close()
                block.unlink()
            except Exception:
                pass
        self._inputs = []

    # ---- Life -------------------------------------------------------------------------------------

    def stderr(self):
        try:
            self.log.seek(0)
            return self.log.read().decode("utf-8", "replace")
        except Exception:
            return ""

    def close(self, timeout=30):
        if self.proc.poll() is None:
            try:
                self.call("shutdown")
            except Exception:
                pass
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for block in self._blocks.values():
            try:
                block.close()
            except Exception:
                pass
        self._blocks = {}
        try:
            self.proc.stdout.close()
        except Exception:
            pass
        try:
            self.log.close()
        except Exception:
            pass
        return self.proc.returncode

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
