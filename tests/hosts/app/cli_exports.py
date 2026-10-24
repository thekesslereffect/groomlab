# SPDX-License-Identifier: Apache-2.0
"""The app's scripted exports and card bake, as ctest runs them (label quick): GroomLab.exe on
the sample groom (a Cards node added through a graph file) writes every target and the bake
without a window, and each file is there and readable enough to tell its format.

    python cli_exports.py <GroomLab.exe> <strokes.groom> <work folder>
"""

import json
import os
import struct
import subprocess
import sys


def run(exe, args):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    proc = subprocess.run([exe] + args, capture_output=True, text=True, env=env, timeout=600)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("GroomLab %s failed with %d" % (" ".join(args[:3]), proc.returncode))
    return proc.stdout + proc.stderr


def main(exe, groom, work):
    os.makedirs(work, exist_ok=True)
    # The sample's graph with a Cards node before the output: the sample holds the default
    # graph as "{}", so the app saves it first (it writes the graph as the core does, every
    # node with every param) and the Cards node is a short addition to that.
    saved = os.path.join(work, "full.groom")
    # (--save alone keeps the window; an export makes the run headless.)
    run(exe, [groom, "--save", saved, "--export-alembic", os.path.join(work, "first.abc")])
    with open(saved, encoding="utf-8") as f:
        graph = json.load(f)["graph"]
    output = graph["output"]
    for c in graph["connections"]:
        if c["to"] == output and c["input"] == "strands":
            c["to"] = "cards"
    graph["nodes"].append({"name": "cards", "type": "Cards", "params": {"variations": 8, "atlasSize": 256}})
    graph["connections"].append({"from": "cards", "to": output, "input": "strands"})
    graph_path = os.path.join(work, "cards_graph.json")
    with open(graph_path, "w", encoding="utf-8") as f:
        json.dump(graph, f)

    abc = os.path.join(work, "cli.abc")
    usda = os.path.join(work, "cli.usda")
    unreal = os.path.join(work, "cli_unreal.abc")
    unity = os.path.join(work, "cli_unity.abc")
    glb = os.path.join(work, "cli_cards.glb")
    vrscene = os.path.join(work, "cli.vrscene")
    bake = os.path.join(work, "bake")
    log = run(exe, [groom, "--graph", graph_path, "--export-alembic", abc, "--export-usd", usda, "--export-unreal", unreal,
                    "--export-unity", unity, "--export-vrscene", vrscene, "--points-per-strand", "6", "--export-cards", glb, "--cards-resolution", "128",
                    "--bake-cards", bake, "--bake-name", "cli"])
    for path in (abc, usda, unreal, unity, glb, vrscene):
        assert os.path.getsize(path) > 0, path
    with open(vrscene, encoding="utf-8") as f:
        text = f.read()
    for plugin in ("GeomMayaHair cli@hair", "BRDFHair4 cli@hairBrdf", "Node cli@node"):
        assert plugin in text, "a V-Ray scene with " + plugin
    with open(abc, "rb") as f:
        assert f.read(5) == b"Ogawa", "an Ogawa Alembic archive"
    with open(usda, encoding="utf-8") as f:
        text = f.read()
    assert "def BasisCurves" in text and "primvars:st" in text, "a USD text layer with curves"
    with open(glb, "rb") as f:
        magic, version, length = struct.unpack("<4sII", f.read(12))
        assert magic == b"glTF" and version == 2, "a glTF 2 binary"
        chunk_length, chunk_type = struct.unpack("<I4s", f.read(8))
        doc = json.loads(f.read(chunk_length))
    assert chunk_type == b"JSON" and len(doc["images"]) >= 2 and doc["meshes"][0]["name"].endswith("_cards"), doc.get("meshes")
    for m in ("Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal"):
        tga = os.path.join(bake, "cli_%s.tga" % m)
        with open(tga, "rb") as f:
            header = f.read(18)
        assert struct.unpack("<H", header[12:14])[0] == 128, tga
    assert os.path.exists(os.path.join(bake, "cli_Atlas.json")) and os.path.exists(os.path.join(bake, "cli_Preview.png"))
    assert "Wrote" in log and "Baked" in log, log

    # Simulate, bake, save: a Simulate node between the guides and Interpolate, baked to frame 6
    # beside the groom (which is in the work folder), the saved groom naming the bake.
    with open(saved, encoding="utf-8") as f:
        graph = json.load(f)["graph"]
    for c in graph["connections"]:
        if c["input"] == "guides" and next(n for n in graph["nodes"] if n["name"] == c["to"])["type"] == "Interpolate":
            guides_from, c["from"] = c["from"], "sim"
    graph["nodes"].append({"name": "sim", "type": "Simulate", "params": {"gravity": 9.8}})
    graph["connections"].append({"from": guides_from, "to": "sim", "input": "guides"})
    sim_graph = os.path.join(work, "sim_graph.json")
    with open(sim_graph, "w", encoding="utf-8") as f:
        json.dump(graph, f)
    sim_saved = os.path.join(work, "sim.groom")
    log = run(exe, [saved, "--graph", sim_graph, "--bake-simulation", "6", "--save", sim_saved,
                    "--export-alembic", os.path.join(work, "sim_out.abc")])
    assert "Baked the simulation to frame 6" in log, log
    with open(sim_saved, encoding="utf-8") as f:
        sims = json.load(f)["simCaches"]
    assert sims["sim"]["file"] == "full_sim/full_sim.abc" and len(sims["sim"]["stamp"]) == 16, sims
    with open(os.path.join(work, "full_sim", "full_sim.abc"), "rb") as f:
        assert f.read(5) == b"Ogawa", "the bake is an Ogawa Alembic archive"
    print("cli exports: ok")


if __name__ == "__main__":
    main(*sys.argv[1:4])
