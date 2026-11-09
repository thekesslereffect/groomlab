# SPDX-License-Identifier: Apache-2.0
"""What GroomLab's standalone renders share, whatever the renderer (V-Ray Standalone: vray_scene,
RenderMan's prman: prman_scene): finding the renderer's executable, the frames and the images a
render writes, values over frames, the camera's field of view, the lights a scene without any
gets, and the renderer running in the background (RenderJob). No Maya here: render_gather reads
the Maya scene into the plain data these take, and tests/hosts/vray and tests/hosts/renderman check
them without Maya.

Values over frames ("samples"): one value for every frame, or [(frame, value), ...] in frame order.
A light, mesh or camera from render_gather.gather holds its matrix (and what else changes) so.
"""

import glob
import math
import os
import re
import signal
import subprocess
import sys
import time


# ---------------------------------------------------------------------------
# Finding a renderer's executable

def exe_in(path, exe):
    """`exe` at `path` (the file, its folder, or an install folder with bin/), or None."""
    if not path:
        return None
    path = os.path.expandvars(os.path.expanduser(str(path).strip().strip('"')))
    if os.path.isfile(path):
        return os.path.abspath(path)
    for candidate in (os.path.join(path, exe), os.path.join(path, "bin", exe)):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    return None


def find_exe(exe, candidates, environ=None):
    """The first of `candidates` (files or folders, None skipped) holding `exe`, else `exe` on the
    PATH; None when there is none."""
    environ = os.environ if environ is None else environ
    for candidate in candidates:
        found = exe_in(candidate, exe)
        if found:
            return found
    for folder in environ.get("PATH", "").split(os.pathsep):
        found = exe_in(os.path.join(folder, exe), exe) if folder else None
        if found:
            return found
    return None


# ---------------------------------------------------------------------------
# Frames, images, names

def frame_list(start, end=None, step=1):
    """The frames rendered, start to end (inclusive) by step."""
    end = start if end is None else end
    step = max(1, int(step))
    return list(range(int(round(start)), int(round(end)) + 1, step))


def images_for(image, frames):
    """The files a render of `frames` to `image` writes, in order: `image` for one frame, else
    <image stem>.<frame, 4 digits><ext>."""
    if len(frames) == 1:
        return [image]
    stem, ext = os.path.splitext(image)
    return ["%s.%04d%s" % (stem, int(f), ext) for f in frames]


def latest_image(folder):
    """The newest PNG in `folder`, or None."""
    found = glob.glob(os.path.join(folder, "*.png"))
    return max(found, key=os.path.getmtime) if found else None


def safe_name(text):
    """A name renderers take as it is: letters, digits and '_' (vrscene_writer.h vray::pluginName's rule)."""
    name = re.sub(r"[^A-Za-z0-9_]", "_", text or "")
    if not name:
        return "node"
    return "_" + name if name[0].isdigit() else name


# ---------------------------------------------------------------------------
# Values over frames, matrices

IDENTITY = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def is_keyed(samples):
    return isinstance(samples, list) and bool(samples) and isinstance(samples[0], tuple)


def first(samples):
    """The first frame's value of `samples`."""
    return samples[0][1] if is_keyed(samples) else samples


def mapped(samples, fn):
    """`fn` of each frame's value of `samples`."""
    if is_keyed(samples):
        return [(f, fn(v)) for f, v in samples]
    return fn(samples)


def at(samples, frame):
    """The value of `samples` at `frame` (the nearest frame written at or before it)."""
    if not is_keyed(samples):
        return samples
    value = samples[0][1]
    for f, v in samples:
        if f <= frame + 1e-6:
            value = v
    return value


def length(v):
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5


def unit(v):
    n = length(v)
    return [c / n for c in v] if n > 1e-12 else [0.0, 0.0, 1.0]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def unscaled(m):
    """(the matrix with unit axes, the axes' lengths): a light's or a camera's size apart. Maya's
    world matrix: 16 values, row-major, each row an axis' image, the translation in the last row."""
    lengths = [length(m[4 * k:4 * k + 3]) for k in range(3)]
    out = list(m)
    for k in range(3):
        s = lengths[k] if lengths[k] > 1e-12 else 1.0
        for i in range(3):
            out[4 * k + i] = m[4 * k + i] / s
    return out, lengths


def inverse_rigid(m):
    """The inverse of a matrix whose axes are orthonormal (a camera's, unscaled): the rotation
    transposed and the translation taken back."""
    r = [[m[4 * i + j] for j in range(3)] for i in range(3)]
    t = m[12:15]
    out = [r[j][i] for i in range(3) for j in range(3)]  # rows of the transpose
    inv = [out[0], out[1], out[2], 0.0, out[3], out[4], out[5], 0.0, out[6], out[7], out[8], 0.0]
    inv += [-sum(t[k] * r[i][k] for k in range(3)) for i in range(3)] + [1.0]
    return inv


# ---------------------------------------------------------------------------
# Cameras and lights

def horizontal_fov(focal, aperture_w, aperture_h, aspect, fit="horizontal", camera_scale=1.0, overscan=1.0):
    """The horizontal field of view (radians) of a Maya camera rendering at image `aspect` (width /
    height): `focal` in mm, the film back in inches, Maya's film fit (fill, horizontal, vertical,
    overscan) choosing which side of the film the image's matches."""
    aw, ah = aperture_w * 25.4 * camera_scale, aperture_h * 25.4 * camera_scale
    film_aspect = aw / max(ah, 1e-9)
    if fit == "fill":
        fit = "horizontal" if film_aspect <= aspect else "vertical"
    elif fit == "overscan":
        fit = "vertical" if film_aspect <= aspect else "horizontal"
    if fit == "vertical":
        return 2.0 * math.atan(0.5 * ah * aspect * overscan / focal)
    return 2.0 * math.atan(0.5 * aw * overscan / focal)


def default_lights(camera_matrix):
    """Lights for a scene with none a renderer can take: a soft grey dome and a key from above the
    camera's left shoulder (a distant light: no distances to guess). Light dicts as
    render_gather.gather writes them: kind "dome" (radiance) and "direct" (irradiance, Arnold's)."""
    m = unscaled(camera_matrix)[0]
    right, cam_up, back = m[0:3], m[4:7], m[8:11]
    # Toward the light: back toward the camera, up, and to the camera's left.
    toward = [0.6 * back[i] + 0.6 * cam_up[i] - 0.5 * right[i] for i in range(3)]
    z = unit(toward)
    helper = [0.0, 1.0, 0.0] if abs(z[1]) < 0.9 else [1.0, 0.0, 0.0]
    x = unit(cross(helper, z))
    y = cross(z, x)
    key = x + [0.0] + y + [0.0] + z + [0.0] + [0.0, 0.0, 0.0, 1.0]
    return [{"kind": "dome", "name": "groomlab_default_dome", "color": (0.3, 0.3, 0.3), "intensity": 1.0},
            {"kind": "direct", "name": "groomlab_default_key", "matrix": key, "color": (1.0, 0.97, 0.92),
             "intensity": 3.0}]


# ---------------------------------------------------------------------------
# The renderer in the background


def kill_tree(process):
    """Kills `process` and what it started (off Windows). macOS: Maya's Render is a shell script that
    starts Maya in batch mode, so killing the script alone left the render running: the renders start
    in their own session (start_new_session) and the whole process group goes."""
    if sys.platform == "darwin":
        try:
            if os.getpgid(process.pid) == process.pid:  # its own group leader: never this Maya's group
                os.killpg(process.pid, signal.SIGKILL)
                return
        except OSError:  # gone already
            return
    process.kill()


class RenderJob(object):
    """A renderer rendering in the background (the host stays free): poll() for progress, cancel()
    to stop it. The images land in `folder` (images_for), the renderer's output in render.log
    beside them. A renderer's job says how its log tells progress (progress_of) and errors
    (is_error)."""

    NO_WINDOW = 0x08000000  # Windows: no console window
    name = "the renderer"

    def __init__(self, cmd, folder, frames, image, scene, env=None):
        self.command, self.folder, self.frames, self.image, self.scene = cmd, folder, list(frames), image, scene
        self.log = os.path.join(folder, "render.log")
        self.started = time.time()
        self.cancelled = False
        with open(self.log, "w") as out:
            self.process = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            cwd=folder, env=env,
                                            creationflags=self.NO_WINDOW if os.name == "nt" else 0,
                                            start_new_session=sys.platform == "darwin")

    @staticmethod
    def progress_of(log_text):
        """(the frame being rendered or None, its percent done) from the log so far."""
        return None, 0.0

    @staticmethod
    def is_error(line):
        return " error: " in line.lower()

    def images(self):
        """The frames' images written so far."""
        return [p for p in images_for(self.image, self.frames) if os.path.isfile(p)]

    def log_text(self):
        try:
            with open(self.log, "r", errors="replace") as f:
                return f.read()
        except (IOError, OSError):
            return ""

    def poll(self):
        """(frames done, frames asked, finished, percent of the current frame)."""
        done = self.process.poll() is not None
        frame, percent = self.progress_of(self.log_text())
        count = len(self.images())
        if not done and count and frame is not None and len(self.frames) > 1:
            count = sum(1 for f in self.frames if f < frame)
        return count, len(self.frames), done, (100.0 if done else percent)

    def ok(self):
        return self.process.poll() == 0 and not self.cancelled and len(self.images()) == len(self.frames)

    def errors(self):
        """The renderer's error lines (the last few)."""
        return [line.strip() for line in self.log_text().splitlines() if self.is_error(line)][-5:]

    def cancel(self):
        """Stop the renderer (and anything it started)."""
        self.cancelled = True
        if self.process.poll() is not None:
            return
        if os.name == "nt":
            subprocess.call(["taskkill", "/F", "/T", "/PID", str(self.process.pid)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=self.NO_WINDOW)
        else:
            kill_tree(self.process)
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            pass

    def wait(self, timeout=None):
        """Block until the renderer is done or `timeout` seconds pass; returns poll()."""
        try:
            self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            pass
        return self.poll()


def clear_images(image, frames):
    """Remove last time's images of a render (a finished check must not count them)."""
    for old in images_for(image, frames):
        if os.path.isfile(old):
            os.remove(old)
