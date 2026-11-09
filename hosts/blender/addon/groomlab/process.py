# SPDX-License-Identifier: GPL-3.0-or-later
"""groomlab-helper's life in a Blender session: started on first use, one per session, started
again when it died (the grooms are sent again: groom.py watches `generation()`), shut down when
the add-on is disabled or Blender quits. Nothing of GroomLab's core is loaded into Blender: the
helper is a program of its own we talk to over pipes and shared memory (docs/BLENDER.md)."""

import os
import sys
import tempfile
import threading

from .helper_client import HelperClient, HelperError

PROTOCOL = 1
EXE = "groomlab-helper.exe" if sys.platform == "win32" else "groomlab-helper"

_client = None
_hello = None
_generation = 0
_lock = threading.RLock()


def helper_path():
    """bin/groomlab-helper beside the add-on (the extension's own copy); GROOMLAB_HELPER for
    developers running the add-on from the sources."""
    override = os.environ.get("GROOMLAB_HELPER")
    if override:
        return override
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", EXE)


def log_path():
    return os.path.join(tempfile.gettempdir(), "groomlab-helper-%d.log" % os.getpid())


def _start():
    global _client, _hello, _generation
    exe = helper_path()
    if not os.path.isfile(exe):
        raise HelperError("missing", "the GroomLab helper is not installed (%s): reinstall the GroomLab extension" % exe)
    if sys.platform != "win32" and not os.access(exe, os.X_OK):
        # Blender unpacks an extension's zip with Python's zipfile, which drops the executable bit.
        try:
            os.chmod(exe, os.stat(exe).st_mode | 0o111)
        except OSError:
            pass  # read-only install: Popen says why
    client = HelperClient(exe, log=log_path())
    try:
        hello = client.call("hello", protocol=PROTOCOL)
    except Exception:
        client.close()
        raise
    _client, _hello = client, hello
    _generation += 1
    return client


def client():
    """The running helper, started (again) when needed."""
    with _lock:
        if _client is not None and _client.alive():
            return _client
        if _client is not None:
            _close_quietly()
        return _start()


def running():
    return _client is not None and _client.alive()


def generation():
    """Bumps each time a helper starts: sessions made by an earlier one are gone."""
    return _generation


def hello():
    return _hello or {}


def call(cmd, **args):
    return client().call(cmd, **args)


def _close_quietly():
    global _client
    try:
        _client.close(timeout=5)
    except Exception:
        pass
    _client = None


def shutdown():
    """Ends the helper (add-on disabled, Blender quitting)."""
    global _hello
    with _lock:
        if _client is not None:
            _close_quietly()
        _hello = None


def kill_for_tests():
    """Ends the helper as a crash would (tests: the add-on starts another)."""
    if _client is not None and _client.alive():
        _client.proc.kill()
        _client.proc.wait()


