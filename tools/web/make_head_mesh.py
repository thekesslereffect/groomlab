# SPDX-License-Identifier: Apache-2.0
"""The website's 3D head (web/public/samples/manny_head.bin): the Manny sample head as a tiny binary.

    python tools/web/make_head_mesh.py

Reads engine/assets/meshes/manny_head.obj (CC BY 4.0, "Manny head by Nick Rutlinh (GroomLab), CC BY 4.0")
and writes its subdivision cage, quantized: the page subdivides it (Catmull-Clark, in
web/components/home/head-mesh.ts) and grows the hair on its scalp. Little endian:

    char[4]  "GLHM"
    uint16   version (1)
    uint16   vertex count
    uint16   face count
    uint16   reserved (0)
    float32  centimetres per unit of the quantized positions
    int16    x, y, z per vertex (centimetres / scale; y up, facing +z)
    uint16   4 vertex indices per face (a triangle's 4th is 65535)

Deterministic: the same OBJ always writes the same bytes. No dependencies.
"""

import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "engine", "assets", "meshes", "manny_head.obj")
OUT = os.path.join(ROOT, "web", "public", "samples", "manny_head.bin")


def main():
    verts = []
    faces = []
    with open(SRC, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v":
                verts.append(tuple(float(t) for t in parts[1:4]))
            elif parts[0] == "f":
                idx = [int(t.split("/")[0]) - 1 for t in parts[1:]]
                if len(idx) not in (3, 4):
                    raise SystemExit(f"only triangles and quads, got a {len(idx)}-gon")
                faces.append(idx)
    biggest = max(abs(c) for v in verts for c in v)
    scale = biggest / 32767.0
    out = bytearray(b"GLHM")
    out += struct.pack("<HHHHf", 1, len(verts), len(faces), 0, scale)
    for v in verts:
        out += struct.pack("<3h", *(max(-32767, min(32767, round(c / scale))) for c in v))
    for face in faces:
        out += struct.pack("<4H", *(face + [65535] * (4 - len(face))))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(out)
    print(f"{OUT}: {len(verts)} vertices, {len(faces)} faces, {len(out)} bytes")


if __name__ == "__main__":
    main()
