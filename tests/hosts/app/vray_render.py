# SPDX-License-Identifier: Apache-2.0
"""The app's V-Ray Standalone renders (Render > Render Frame / Turntable in V-Ray), as ctest runs them
when V-Ray Standalone is installed (label vray): GroomLab.exe on the head groom renders a frame and a
small turntable without a window (--render-vray, --vray-turntable), and the images have the hair:

- the frame: the groom's .vrscene and the stage's scene beside the PNG, the hair covering the head
  region where the app's own screenshot of the same view has it (the camera carried over), brown
  (red over blue) as the groom's look;
- the turntable: one image per frame, the views different.

    python vray_render.py <GroomLab.exe> <vray.exe> <work folder>

GROOMLAB_VRAY points the app at vray.exe. Standard library and Qt's PNG reader only (through the
app: --screenshot), the PNGs decoded here with zlib.
"""

import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "..", "data", "app"))


def run(exe, args, vray):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", GROOMLAB_VRAY=vray)
    proc = subprocess.run([exe] + args, capture_output=True, text=True, env=env, timeout=900)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("GroomLab %s failed with %d" % (" ".join(args[-4:]), proc.returncode))
    return proc.stdout + proc.stderr


def read_png(path):
    """(width, height, rows of (r, g, b) 0-255) of an 8-bit RGB or RGBA PNG."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color in (2, 6), (path, depth, color)
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        prev = line
    return width, height, rows


def hair_pixels(rows, background):
    """The pixels that differ from the background color clearly. (12 of 255: the solid backdrop
    varies by 1 or 2, and the dark brown tips sit close to the app's grey backdrop.)"""
    return [(x, y, p) for y, row in enumerate(rows) for x, p in enumerate(row)
            if max(abs(p[k] - background[k]) for k in range(3)) > 12]


def main(exe, vray, work):
    os.makedirs(work, exist_ok=True)
    groom = [os.path.join(DATA, "head_scalp.obj"), "--graph", os.path.join(DATA, "head_graph.json"), "--size", "160x120"]
    frame = os.path.join(work, "frame.png")
    view = os.path.join(work, "view.png")
    log = run(exe, groom + ["--render-vray", frame], vray)
    assert "(V-Ray)" in log, log
    for path in (frame, os.path.join(work, "frame_groom.vrscene"), os.path.join(work, "frame_scene.vrscene")):
        assert os.path.isfile(path), path
    run(exe, groom + ["--screenshot", view, "--tonemap", "none", "--background", "solid", "--quality", "refine", "--frames", "32"], vray)  # refined: whole strands

    w, h, rows = read_png(frame)
    assert (w, h) == (160, 120), (w, h)
    background = rows[0][0]
    hair = hair_pixels(rows, background)
    assert len(hair) > 0.1 * w * h, "the hair covers %d of %d pixels" % (len(hair), w * h)
    r = sum(p[0] for _, _, p in hair) / len(hair)
    b = sum(p[2] for _, _, p in hair) / len(hair)
    assert r > 1.15 * b, "brown hair (red over blue): r %.1f b %.1f" % (r, b)
    # Where the app draws it (the camera carried over): the two masks overlap.
    _, _, app = read_png(view)
    mine = {(x, y) for x, y, _ in hair}
    theirs = {(x, y) for x, y, _ in hair_pixels(app, app[0][0])}
    overlap = len(mine & theirs) / max(1, len(mine | theirs))
    print("V-Ray frame: hair %d px, mean r %.1f b %.1f; overlap with the app's view %.2f" % (len(hair), r, b, overlap))
    assert overlap > 0.6, overlap

    turn = os.path.join(work, "turn.png")
    run(exe, groom + ["--render-vray", turn, "--vray-turntable", "3"], vray)
    images = [os.path.join(work, "turn.%04d.png" % k) for k in (1, 2, 3)]
    for path in images:
        assert os.path.isfile(path), path
    first, second = read_png(images[0])[2], read_png(images[1])[2]
    differ = sum(1 for y in range(h) for x in range(w) if max(abs(first[y][x][k] - second[y][x][k]) for k in range(3)) > 24)
    assert differ > 0.05 * w * h, "the turntable's frames differ in %d pixels" % differ
    print("vray render: ok")


if __name__ == "__main__":
    main(*sys.argv[1:4])
