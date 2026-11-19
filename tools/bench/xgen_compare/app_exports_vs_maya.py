# SPDX-License-Identifier: Apache-2.0
"""The standalone app beside the Maya benchmark: one table per strand count.

The app's numbers come from its test executable's benchmark (tests/hosts/app/bench_app.cpp: the
bench's head, bend, guides and graph rebuilt without Maya, timed through GroomDocument), written
as JSON when GROOMLAB_APP_BENCH names the file:

    set GROOMLAB_APP_BENCH=results\\2026-10-02_app_exports.json
    build\\wt\\hosts\\app\\RelWithDebInfo\\groomlab_app_tests.exe --gtest_filter=AppBench.*

Then:

    python tools/bench/xgen_compare/app_exports_vs_maya.py results/2026-10-02_groomlab_5e8bc26.json \\
        results/2026-10-02_app_exports.json results/2026-10-02_app_exports_vs_maya.md

Maya is never re-run here: its medians (of 3 runs, min-max in brackets) are read from its results
JSON as run_bench.py wrote them. The app's are one run each.
"""

import json
import subprocess
import sys

# (label, Maya summary label, Maya variant, app key, app variant, unit, format)
ROWS = [
    ("Strands made", "Strands made", "groomlab", "strands", "groomlab", "", "%d"),
    ("Creation, cold (file read + first evaluation)", "Creation, cold (batch: create + first evaluation)", "groomlab",
     "creation_cold_s", "groomlab", "s", "%.2f"),
    ("Clump tweak to evaluated", "Clump tweak to evaluated (batch)", "groomlab", "clump_tweak_ms", "groomlab", "ms", "%.1f"),
    ("Noise tweak to evaluated", "Noise tweak to evaluated (batch)", "groomlab", "noise_tweak_ms", "groomlab", "ms", "%.1f"),
    ("Per-frame evaluation, deforming scalp (no draw)", "Per-frame evaluation, deforming (batch, no draw)", "groomlab",
     "frame_eval_ms", "groomlab", "ms", "%.1f"),
    ("Per-frame evaluation, deforming scalp, followScalp", "Per-frame evaluation, deforming (batch, no draw)", "groomlab_follow",
     "frame_eval_ms", "groomlab_follow", "ms", "%.1f"),
    ("Alembic export, 24 frames", "Alembic export, 24 frames", "groomlab", "alembic_export_s", "groomlab", "s", "%.2f"),
    ("Alembic size, 24 frames", "Alembic size, 24 frames", "groomlab", "alembic_mb", "groomlab", "MB", "%.0f"),
    ("Alembic export, 24 frames, followScalp", "Alembic export, 24 frames", "groomlab_follow", "alembic_export_s",
     "groomlab_follow", "s", "%.2f"),
    ("USD export, 24 frames (text layer)", None, None, "usd_export_s", "groomlab", "s", "%.2f"),
    ("USD size, 24 frames", None, None, "usd_mb", "groomlab", "MB", "%.0f"),
    ("Playback, deforming scalp (Maya: GUI play; app: evaluate + build + draw offscreen, 1280x720)",
     "Playback, deforming scalp, 1st pass (GUI, play)", "groomlab", "play_fps", "groomlab", "fps", "%.1f"),
    ("Playback, deforming scalp, followScalp", "Playback, deforming scalp, 1st pass (GUI, play)", "groomlab_follow",
     "play_fps", "groomlab_follow", "fps", "%.1f"),
]


def maya_cell(summary, label, count, variant, fmt):
    if label is None:
        return "—"
    v = summary.get(label, {}).get(str(count), {}).get(variant)
    if not v:
        return "—"
    unit = v.get("unit", "")
    text = (fmt % v["median"]) + (" " + unit if unit else "")
    if v.get("n", 1) > 1:
        text += " [" + (fmt % v["min"]) + "-" + (fmt % v["max"]) + "]"
    return text


def app_cell(app, count, key, variant, unit, fmt):
    v = app.get("counts", {}).get(str(count), {}).get(variant, {}).get(key)
    if v is None:
        return "—"
    return (fmt % v) + (" " + unit if unit else "")


def git_commit():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
    except Exception:
        return "unknown"


def main(maya_path, app_path, out_path):
    with open(maya_path, encoding="utf-8") as f:
        maya = json.load(f)
    with open(app_path, encoding="utf-8") as f:
        app = json.load(f)
    summary = maya["summary"]
    machine = maya.get("machine", {})

    def name_of(entry):  # run_bench.py keeps the WMI record; its Name is the readable part
        return (entry.get("Name", "?").strip() if isinstance(entry, dict) else str(entry))
    lines = ["# GroomLab standalone app vs the Maya plugin: exports, evaluation and playback", ""]
    lines.append("Maya numbers: %s (GroomLab %s, medians of 3 runs, min-max in brackets), not re-run. App numbers: "
                 "GroomLab %s, one run of `groomlab_app_tests --gtest_filter=AppBench.*` (tests/hosts/app/bench_app.cpp) on the "
                 "same machine (%s, %s)." % (maya.get("tag"), maya.get("groomlab_commit"), git_commit(), name_of(machine.get("cpu", "?")),
                                             name_of(machine.get("gpu", "?"))))
    lines.append("")
    scene = app.get("scene", {})
    lines.append("The app rebuilds the bench scene without Maya: the same head (%d triangles, %.0f cm2, polySphere UVs) bending "
                 "over frames 1-100 as an Alembic scalp cache, %d guides straight along the normals, the same graph "
                 "(Scatter -> Interpolate -> Clump -> Noise -> Curl -> Width -> Resample, 16 points a strand); "
                 "the bend is an approximation of Maya's nonLinear bend (the same handle and keys, the crown swinging about "
                 "5 cm), so the scalp moves as much but not identically. The app evaluates through GroomDocument (the "
                 "GroomScene over the session), as its timeline and export panel do; drawing is offscreen on %s "
                 "(the lookdev path), each frame waited for, where Maya's playback is its viewport's free-running `play`."
                 % (scene.get("triangles", 0), scene.get("area_cm2", 0.0), scene.get("guides", 0), app.get("draw_api", "?")))
    lines.append("")
    for count in maya.get("counts", []):
        if str(count) not in app.get("counts", {}):
            continue
        lines.append("## %d strands, 16 points each" % count)
        lines.append("")
        lines.append("| Measure | Maya plugin | Standalone app |")
        lines.append("| --- | --- | --- |")
        for label, maya_label, maya_variant, key, variant, unit, fmt in ROWS:
            lines.append("| %s | %s | %s |" % (label, maya_cell(summary, maya_label, count, maya_variant, fmt),
                                             app_cell(app, count, key, variant, unit, fmt)))
        lines.append("")
    lines.append("The playback rows are not like for like: the app's headless loop evaluates the frame, builds its display "
                 "buffers and renders it through the offscreen helper, which makes its render target and the lookdev "
                 "pass's own targets again on every call and waits for the GPU, so it is a floor for the window's "
                 "playback (the window keeps its targets; its timeline drops frames to hold the clock), where Maya's "
                 "number is its viewport playing free. The per-frame evaluation rows compare the same work on both sides.")
    lines.append("")
    lines.append("Not compared: the Arnold .ass export (the app has no Arnold translator; the procedural reads the .groom "
                 "file instead), the scene save and open (the app's .groom file is the Maya export), the GUI brush and "
                 "orbit numbers (results/2026-10-02_app_vs_maya.md has the interactive ones).")
    lines.append("")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", out_path)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    main(*sys.argv[1:])
