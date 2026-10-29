# SPDX-License-Identifier: Apache-2.0
"""Writes the brush alphas shipped with every host (hosts/maya/alphas/*.png, 256 x 256 gray).

    python tools/dev/make_alphas.py

Each is drawn from a fixed seed, so running it again gives the same files. White paints, black
does not (brush_alpha.h); the textures fade out toward the circle's edge so a hard brush still
leaves a soft spot.
"""

import math
import os
import random
import struct
import zlib

SIZE = 256
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hosts", "maya", "alphas")


def write_png(path, values):
    """values: SIZE x SIZE floats 0..1, rows top-down -> an 8-bit gray PNG."""
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)
        raw.extend(max(0, min(255, int(round(v * 255)))) for v in values[y * SIZE:(y + 1) * SIZE])

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def uv(x, y):
    """A texel's center in -1..1, v up."""
    return (x + 0.5) / SIZE * 2 - 1, 1 - (y + 0.5) / SIZE * 2


def smoothstep(a, b, t):
    t = max(0.0, min(1.0, (t - a) / (b - a)))
    return t * t * (3 - 2 * t)


def vignette(u, v, inner=0.55, outer=0.95):
    return 1 - smoothstep(inner, outer, math.hypot(u, v))


class ValueNoise:
    """Smooth value noise on a lattice that tiles every `period` cells."""

    def __init__(self, seed, period=64):
        rng = random.Random(seed)
        self.period = period
        self.values = [rng.random() for _ in range(period * period)]

    def at(self, x, y):
        x0, y0 = math.floor(x), math.floor(y)
        tx, ty = x - x0, y - y0
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        p = self.period

        def lattice(i, j):
            return self.values[(j % p) * p + (i % p)]

        top = lattice(x0, y0) + (lattice(x0 + 1, y0) - lattice(x0, y0)) * tx
        bottom = lattice(x0, y0 + 1) + (lattice(x0 + 1, y0 + 1) - lattice(x0, y0 + 1)) * tx
        return top + (bottom - top) * ty

    def fbm(self, x, y, octaves, gain=0.5):
        total, amplitude, norm = 0.0, 1.0, 0.0
        for _ in range(octaves):
            total += amplitude * self.at(x, y)
            norm += amplitude
            amplitude *= gain
            x, y = x * 2.03, y * 2.03
        return total / norm


def image(f):
    return [f(*uv(x, y)) for y in range(SIZE) for x in range(SIZE)]


def dots(seed, count, rmin, rmax, softness):
    """Round dots (radii in texels) scattered inside the circle, `softness` of each radius faded."""
    rng = random.Random(seed)
    values = [0.0] * (SIZE * SIZE)
    for _ in range(count):
        while True:
            cx, cy = rng.uniform(-0.9, 0.9), rng.uniform(-0.9, 0.9)
            if math.hypot(cx, cy) < 0.88:
                break
        r = rng.uniform(rmin, rmax)
        px, py = (cx + 1) / 2 * SIZE, (1 - cy) / 2 * SIZE
        for y in range(int(py - r - 1), int(py + r + 2)):
            for x in range(int(px - r - 1), int(px + r + 2)):
                if 0 <= x < SIZE and 0 <= y < SIZE:
                    d = math.hypot(x + 0.5 - px, y + 0.5 - py) / r
                    w = 1 - smoothstep(1 - softness, 1, d)
                    values[y * SIZE + x] = max(values[y * SIZE + x], w)
    return values


def noise(u, v, n, scale, octaves, low, high):
    return smoothstep(low, high, n.fbm((u + 1) * scale, (v + 1) * scale, octaves)) * vignette(u, v)


def streaks(seed):
    """Fine lines along u (the stroke's way with Rotation: Stroke), each its own brightness and
    length, fading at both ends."""
    rng = random.Random(seed)
    values = [0.0] * (SIZE * SIZE)
    for _ in range(140):
        y = rng.uniform(0.08, 0.92) * SIZE
        half = rng.uniform(0.25, 0.7) * SIZE / 2
        cx = SIZE / 2 + rng.uniform(-0.15, 0.15) * SIZE
        width = rng.uniform(0.6, 1.6)
        bright = rng.uniform(0.45, 1.0)
        for x in range(int(cx - half), int(cx + half)):
            if not 0 <= x < SIZE:
                continue
            along = 1 - abs(x + 0.5 - cx) / half
            for row in range(int(y - width - 1), int(y + width + 2)):
                if 0 <= row < SIZE:
                    w = max(0.0, 1 - abs(row + 0.5 - y) / width) * smoothstep(0, 0.35, along) * bright
                    values[row * SIZE + x] = max(values[row * SIZE + x], w)
    return [v * vignette(*uv(i % SIZE, i // SIZE), 0.6, 1.0) for i, v in enumerate(values)]


def cells(seed):
    """Voronoi cells, bright at their centers and dark along their edges: clumps of hair."""
    rng = random.Random(seed)
    points = [(rng.uniform(-1.1, 1.1), rng.uniform(-1.1, 1.1)) for _ in range(28)]

    def at(u, v):
        d = sorted((u - px) ** 2 + (v - py) ** 2 for px, py in points)
        edge = math.sqrt(d[1]) - math.sqrt(d[0])
        return smoothstep(0.0, 0.18, edge) * vignette(u, v)

    return image(at)


def main():
    os.makedirs(OUT, exist_ok=True)
    noise_a, noise_b, noise_c = ValueNoise(11), ValueNoise(23), ValueNoise(37, period=128)
    alphas = {
        "Speckle": dots(5, 420, 1.2, 3.2, 0.5),
        "Spots": dots(9, 26, 9, 24, 0.8),
        "Noise": image(lambda u, v: noise(u, v, noise_a, 6, 4, 0.35, 0.7)),
        "Clouds": image(lambda u, v: noise(u, v, noise_b, 2.2, 4, 0.3, 0.75)),
        "Grain": image(lambda u, v: noise(u, v, noise_c, 40, 2, 0.45, 0.6)),
        "Streaks": streaks(17),
        "Cells": cells(31),
        "Square": image(lambda u, v: 1.0 if max(abs(u), abs(v)) <= 0.68 else 0.0),
    }
    for name, values in alphas.items():
        write_png(os.path.join(OUT, name + ".png"), values)
        print("wrote", name)


if __name__ == "__main__":
    main()
