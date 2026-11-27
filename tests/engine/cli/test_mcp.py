# SPDX-License-Identifier: Apache-2.0
"""groomlab-cli's MCP server played as an AI client plays it: a scripted session over stdin/stdout.

    python test_mcp.py <groomlab-cli> <work folder>

Starts `groomlab-cli mcp --root <work folder>`, then: initialize, tools/list (every tool has a
schema), the discovery tools, new_groom on the Nika sample, apply_preset, add_node, set_params,
sample_scalp, add_guides, set_tube, add_script_node with an error and then fixed, evaluate (no
errors), render_preview (a real PNG with something drawn, the same twice), save_groom and open_groom
(the same strands), export USD, save_node, a path outside the root refused, undo. Then a groom by
hand: scalp_landmarks, guides placed along the scalp, combed from the crown to the nape, cut,
density painted, measure_guides, previews from two cameras (guides drawn, before and after), undo,
redo and history, saved and opened again. Then everything else an artist does: a photo rendered from
a known camera added, lined up (the solved camera checked against the truth) and guides grown from
it; lock guides from strokes drawn on the photo (the hair's shell, filled in, reshaped, a haircut
combed in); a root tube drawn, subdivided and split a level down; an expression checked and set; a head turn
written as an animated scalp and simulated, the frames baked; a turntable in another look; hair cards
exported for glTF and Unreal. Standard library only.
"""

import base64
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import zlib


class Client:
    def __init__(self, exe, root):
        self.log = open(os.path.join(root, "server_stderr.txt"), "wb")
        self.proc = subprocess.Popen([exe, "mcp", "--root", root], cwd=root, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log)
        self.next_id = 1

    def send(self, message):
        self.proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        line = self.proc.stdout.readline()
        if not line:
            raise AssertionError("the server closed stdout (see server_stderr.txt)")
        response = json.loads(line)
        assert response.get("jsonrpc") == "2.0" and response.get("id") == rid, response
        return response

    def call(self, name, arguments=None, expect_error=False):
        response = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        assert "result" in response, response
        result = response["result"]
        if result.get("isError") != expect_error:
            raise AssertionError("%s %s: %s" % (name, "succeeded" if expect_error else "failed", json.dumps(result)[:2000]))
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        data = json.loads(texts[-1]) if texts else None
        return data, result

    def close(self):
        self.proc.stdin.close()
        code = self.proc.wait(timeout=60)
        self.log.close()
        return code


def png_pixels(data):
    """Width, height and the RGB rows of an 8-bit RGB PNG (any filter)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color == 2, "an 8-bit RGB PNG"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 3
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
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
        rows.append(bytes(line))
        prev = line
    return width, height, rows


def image_of(result, index=0):
    images = [x for x in result["content"] if x["type"] == "image"]
    return png_pixels(base64.b64decode(images[index]["data"]))


def groom_by_hand(c):
    """A groom made the way an artist grooms: landmarks, strokes, painting, and checks in numbers and pictures."""
    g = {"groom": "head"}
    c.call("new_groom", {"name": "head", "mesh": "nika"})
    marks, _ = c.call("scalp_landmarks", g)
    lm = marks["landmarks"]
    for name in ["top", "crown", "front_hairline", "nape", "left_temple", "right_temple", "left_ear", "right_ear"]:
        assert name in lm, name
    assert marks["upAxis"] == "Y", marks
    assert lm["top"]["p"][1] >= lm["crown"]["p"][1] > lm["nape"]["p"][1], lm
    assert lm["crown"]["p"][2] < lm["top"]["p"][2] < lm["front_hairline"]["p"][2], lm  # front is +Z
    assert lm["left_temple"]["p"][0] > 0 > lm["right_temple"]["p"][0], lm  # the character's left: +X

    # Guides placed along the scalp: front to back over the top, then ear to ear.
    placed, _ = c.call("brush_stroke", dict(g, mode="place", **{"from": "front_hairline", "via": ["top", "crown"], "to": "nape",
                                                               "length": 14, "spacing": 2.5, "match_neighbors": False}))
    across, _ = c.call("brush_stroke", dict(g, mode="place", **{"from": "left_ear", "via": ["crown"], "to": "right_ear",
                                                               "length": 14, "spacing": 2.5}))
    assert placed["changed"]["guidesAdded"] >= 8 and across["changed"]["guidesAdded"] >= 8, (placed, across)
    count = placed["changed"]["guidesAdded"] + across["changed"]["guidesAdded"]
    standing, _ = c.call("measure_guides", g)
    assert standing["groups"][0]["count"] == count, standing

    # Combed from the crown to the nape, off the scalp to reach along the guides: they lean back, lengths kept.
    combed, _ = c.call("brush_stroke", dict(g, mode="comb", **{"from": "crown", "to": "nape", "offset": 3, "radius": 6}))
    ch = combed["changed"]
    assert ch["guidesTouched"] > 0 and ch["maxMove"] > 1, combed
    assert abs(ch["meanLengthAfter"] - ch["meanLengthBefore"]) < 0.1, combed
    leaning, _ = c.call("measure_guides", g)
    assert leaning["groups"][0]["direction"][2] < standing["groups"][0]["direction"][2], (standing, leaning)  # more to -Z

    # Cut where a band over the back crosses them: shorter.
    cut, _ = c.call("brush_stroke", dict(g, mode="cut", **{"from": "left_ear", "via": ["back"], "to": "right_ear", "offset": 6,
                                                          "radius": 4}))
    assert cut["changed"]["guidesTouched"] > 0 and cut["changed"]["meanLengthAfter"] < cut["changed"]["meanLengthBefore"], cut

    # Density painted away along the nape: fewer strands, the map's values say so.
    dense, _ = c.call("paint_map", dict(g, **{"for": "scatter.density", "fill": 1}))
    thin, _ = c.call("paint_map", dict(g, **{"for": "scatter.density", "from": "left_ear", "via": ["nape"], "to": "right_ear",
                                            "value": 0, "radius": 4}))
    assert thin["values"]["min"] == 0 and thin["values"]["max"] == 1 and thin["verticesPainted"] > 0, thin
    assert thin["summary"]["strands"] < dense["summary"]["strands"], (dense, thin)

    # Selected about the crown: the selection measured on its own.
    picked, _ = c.call("select_guides", dict(g, center="crown", radius=5))
    assert picked["selected"] > 0, picked
    crown, _ = c.call("measure_guides", dict(g, selection=True))
    assert crown["groups"][0]["count"] == picked["selected"], crown

    # Looked at from two cameras with the guides drawn; then before and after the last change.
    info, result = c.call("render_preview", dict(g, views=["back", "left"], size=256, guides=True))
    width, height, rows = image_of(result)
    assert (width, height) == (256, 128), (width, height)
    orange = sum(1 for row in rows for i in range(0, len(row), 3) if row[i] > 200 and 90 < row[i + 1] < 220 and row[i + 2] < 90)
    assert orange > 50, "the guides are drawn (%d orange pixels)" % orange
    info, result = c.call("render_preview", dict(g, views=["back"], size=128, compare=True))
    assert len([x for x in result["content"] if x["type"] == "image"]) == 2 and "before" in info["images"][0], info
    assert image_of(result, 0)[2] != image_of(result, 1)[2], "the density stroke shows"

    # Undo, redo and the history.
    history, _ = c.call("history", g)
    titles = [s["title"] for s in history["steps"]]
    paint = "paint map " + thin["map"]
    assert titles == ["place stroke", "place stroke", "comb stroke", "cut stroke", paint, paint], titles
    undone, _ = c.call("undo", dict(g, steps=2))
    assert undone["undone"] == [paint, paint] and undone["summary"]["strands"] == cut["summary"]["strands"], undone
    redone, _ = c.call("redo", dict(g, steps=2))
    assert redone["redone"] == [paint, paint] and redone["summary"]["strands"] == thin["summary"]["strands"], (redone, thin)
    c.call("redo", g, expect_error=True)

    # Saved and opened again: the same guides and strands.
    c.call("save_groom", dict(g, path="out/head.groom"))
    again, _ = c.call("open_groom", {"path": "out/head.groom", "name": "head2"})
    assert again["summary"]["strands"] == thin["summary"]["strands"], again
    reread, _ = c.call("measure_guides", {"groom": "head2"})
    final, _ = c.call("measure_guides", g)
    assert reread["groups"][0]["count"] == final["groups"][0]["count"], (reread, final)
    assert abs(reread["groups"][0]["length"]["mean"] - final["groups"][0]["length"]["mean"]) < 1e-3, (reread, final)


def vsub(a, b):
    return [a[i] - b[i] for i in range(3)]


def vdot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def vcross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def vnorm(a):
    length = math.sqrt(vdot(a, a))
    return [x / length for x in a]


def everything_else(c, root):
    """Photos, roots and tubes, expressions, simulation, looks and game exports."""
    # A photo of a groom from a known camera: render_preview through it, saved as the photo.
    c.call("new_groom", {"name": "model", "mesh": "nika"})
    spots, _ = c.call("sample_scalp", {"groom": "model", "count": 60, "min_up": 0.2})
    c.call("add_guides", {"groom": "model", "guides": [{"root": p["p"], "length": 16, "droop": 0.85} for p in spots["points"]]})
    eye, target, focal_mm, width, height = [10.0, 8.0, 75.0], [0.0, 2.0, 0.0], 50.0, 480, 480
    shot, _ = c.call("render_preview", {"groom": "model", "size": width, "save_to": "photos/front.png",
                                        "camera": {"eye": eye, "target": target, "focal_mm": focal_mm, "width": width, "height": height}})
    assert shot["width"] == width and shot["height"] == height, shot
    back = vnorm(vsub(eye, target))
    right = vnorm(vcross([0, 1, 0], back))
    up = vcross(back, right)
    focal_px = focal_mm / 36.0 * width

    def project(p):
        d = vsub(p, eye)
        depth = -vdot(d, back)
        return [width / 2 + focal_px * vdot(d, right) / depth, height / 2 - focal_px * vdot(d, up) / depth]

    g = {"groom": "art"}
    c.call("new_groom", {"name": "art", "mesh": "nika"})
    added, result = c.call("add_photo", dict(g, path="photos/front.png"))
    assert added["width"] == width and image_of(result)[0] == width, added
    seen, result = c.call("mesh_landmarks", dict(g, view="front", photo="front", count=16))
    assert len(seen["points"]) == 16 and image_of(result)[0] == width, seen
    # The pairs an agent reads off the two pictures: here the truth's projection of each point.
    pairs = [{"pixel": project(p["p"]), "point": p["p"]} for p in seen["points"]]
    pairs = [q for q in pairs if 0 < q["pixel"][0] < width and 0 < q["pixel"][1] < height][:10]
    assert len(pairs) >= 6, pairs
    aligned, result = c.call("align_photo", dict(g, pairs=pairs))
    assert aligned["linedUp"] and aligned["rmsPixels"] < 0.5, aligned
    assert max(abs(aligned["camera"]["eye"][i] - eye[i]) for i in range(3)) < 0.1, (aligned, eye)
    assert abs(aligned["camera"]["focal_mm"] - focal_mm) < 0.5, aligned
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    c.call("photo_edit", dict(g, op="add_flow", points=[[240, 120], [250, 300]]))
    viewed, result = c.call("photo_view", dict(g, analysis=True))
    assert viewed["linedUp"] and viewed["flowArrows"] == 1 and viewed["locks"] >= 0, viewed
    grown, _ = c.call("guides_from_photos", dict(g, guide_count=120))
    assert grown["guides"] > 20 and grown["group"] == "fromReference", grown
    compared, result = c.call("render_preview", dict(g, camera={"photo": "front"}, size=240))
    assert compared["width"] == 240 and compared["height"] == 240, compared
    shown, _ = c.call("render_preview", dict(g, camera={"photo": "front"}))
    assert shown["width"] == width and shown["height"] == height, shown  # (the camera's own size)

    # A whole head: its facial landmarks by name, paired by name with a view hint and refined by
    # the outline; the scalp morphed to the photo and reset; the scalp from the landmarks; the look.
    f = {"groom": "face"}
    made, _ = c.call("new_groom", {"name": "face", "mesh": "nika"})
    assert made["head"]["face"] and "scalp_from_landmarks" in made["head"]["note"], made
    c.call("add_photo", dict(f, path="photos/front.png"))
    zoom, result = c.call("photo_view", dict(f, region=[180, 150, 300, 260]))
    assert zoom["grid"] in (5, 10) and zoom["region"] == [180, 150, 300, 260] and image_of(result)[0] == zoom["shown"][0], zoom
    named, _ = c.call("mesh_landmarks", dict(f, view="front", photo="front", count=8))
    names = {n["name"]: n for n in named["named"]}
    assert {"nose_tip", "chin", "left_eye", "right_eye", "left_mouth_corner"} <= set(names), sorted(names)
    picks = ["nose_tip", "chin", "left_eye", "right_eye", "left_mouth_corner", "right_mouth_corner", "left_eye_outer", "right_eye_outer"]
    by_name = [{"pixel": project(names[n]["p"]), "point": n} for n in picks if n in names]
    lined, result = c.call("align_photo", dict(f, pairs=by_name, view="front", refine=True))
    assert lined["linedUp"] and lined["rmsPixels"] < 1.0, lined
    assert "refine" in lined and isinstance(lined["refine"]["done"], bool), lined
    assert "viewAngleFromHint" in lined, lined
    together, result = c.call("align_photos", dict(f, photos=[{"photo": "front", "view": "front", "pairs": by_name}]))
    shot = together["photos"][0]
    assert together["focal_mm"] > 20 and shot["rmsPixels"] < 2.0 and abs(shot["yaw"]) < 30, together
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    morphed, result = c.call("morph_to_photos", dict(f, stiffness=0.2))
    assert morphed["pins"] >= 6 and morphed["photos"][0]["photo"] == "front", morphed
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    c.call("morph_to_photos", dict(f, reset=True))
    region, _ = c.call("scalp_from_landmarks", f)
    assert region["linked"].endswith(".density") and 0.05 < region["scalpShare"] < 0.7, region
    grown, _ = c.call("guides_from_photos", dict(f, guide_count=80, apply_look=True))
    assert grown["guides"] > 5 and "unseenShare" in grown, grown
    assert "lookApplied" in grown or "look" not in grown, grown
    # The groom against its photo: the silhouettes' overlap, where the hair differs, its color.
    compared, result = c.call("compare_to_photo", f)
    shot = compared["photos"][0]
    assert shot["photo"] == "front" and 0.0 <= shot["iou"] <= 1.0, compared
    assert set(shot["regions"]) == {"top", "right", "lower", "left"}, shot
    assert 0.0 <= shot["color"]["photo"]["melanin"] <= 1.0 and 0.0 <= shot["color"]["groom"]["melanin"] <= 1.0, shot
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    one, _ = c.call("compare_to_photo", dict(f, photo="front"))
    assert len(one["photos"]) == 1, one
    # The groom beside its photo, its guides labelled, a map over the scalp, pairs suggested.
    beside, result = c.call("render_preview", dict(f, beside="front"))
    assert beside["beside"] == "front" and beside["width"] == 2 * width and image_of(result)[0] == 2 * width, beside
    labelled, _ = c.call("render_preview", dict(f, views=["front"], label_guides="fromReference", size=256))
    assert labelled["guidesLabelled"]["count"] > 0, labelled
    shown, result = c.call("show_map", dict(f, expression="$y", views=["front"], size=256))
    assert shown["values"]["max"] > shown["values"]["min"] and len([x for x in result["content"] if x["type"] == "image"]) == 1, shown
    suggested, result = c.call("suggest_pairs", dict(f, photo="front", landmarks=["nose_tip", "chin", "left_eye"]))
    seen = [p for p in suggested["pairs"] if p["seen"]]
    assert len(seen) == 3 and all(0.0 <= p["confidence"] <= 1.0 for p in seen), suggested
    near = project(names["nose_tip"]["p"])
    nose = [p for p in seen if p["landmark"] == "nose_tip"][0]
    assert abs(nose["predicted"][0] - near[0]) < 8 and abs(nose["predicted"][1] - near[1]) < 8, (nose, near)
    # A checkpoint, a variant, back again in one step; a batch of calls in one request.
    c.call("checkpoint", dict(f, name="grown"))
    c.call("set_params", dict(f, node="color", params={"melanin": 0.1}))
    restored, _ = c.call("restore", dict(f, name="grown"))
    assert "graph" in restored["changed"], restored
    ran, _ = c.call("batch", {"calls": [{"tool": "evaluate", "arguments": f}, {"tool": "compare_to_photo", "arguments": f}]})
    assert ran["ran"] == 2 and all(x["ok"] for x in ran["calls"]), ran

    # Lock guides: the hair's shell from the photo, strokes drawn on it lifted to guides (filled in,
    # the lock look), a guide reshaped in place, a haircut combed in, pixels cast onto the head.
    shell, result = c.call("hair_shell", dict(f, image=True))
    assert shell["from"] == "photos" and shell["photos"] == 1 and shell["head"]["radius"] > 0, shell
    assert set(shell["regions"]) == {"top", "front", "sides", "back", "nape"}, shell
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    spots, _ = c.call("sample_scalp", dict(f, region="hair", count=50, min_spacing=2.0))
    assert spots["region"] == "hair" and 5 < len(spots["points"]) <= 50, spots
    cast, _ = c.call("photo_project", dict(f, pixels=[project(names["nose_tip"]["p"])], points=[names["nose_tip"]["p"]]))
    assert cast["hits"][0]["hit"] and not cast["hits"][0]["hairRegion"], cast
    assert abs(cast["projected"][0]["pixel"][0] - project(names["nose_tip"]["p"])[0]) < 1.5 and cast["projected"][0]["visible"], cast
    listed, _ = c.call("get_guides", dict(f, group="fromReference", with_points=True, limit=3))
    strokes = [[project(q) for q in gd["positions"]] for gd in listed["guides"][:2]]
    lifted, result = c.call("guides_from_strokes", dict(f, photo="front", strokes=strokes, mirror="x", fill=True, fill_count=40, apply_look=True))
    assert len(lifted["ids"]) == 4 and all(0.0 <= s["hitShare"] <= 1.0 for s in lifted["strokes"]), lifted
    assert lifted["fill"]["guides"] > 10 and "subClump" in lifted["lookApplied"], lifted
    assert len([x for x in result["content"] if x["type"] == "image"]) == 1
    one = c.call("get_guides", dict(f, ids=[lifted["ids"][0]], with_points=True))[0]["guides"][0]
    moved = [[p[0], p[1] + 1.0, p[2]] for p in one["positions"]]
    reshaped, _ = c.call("reshape_guides", dict(f, guides=[{"id": one["id"], "points": moved}]))
    again = c.call("get_guides", dict(f, ids=[one["id"]]))[0]["guides"][0]
    assert reshaped["count"] == 1 and again["group"] == "strokes" and again["id"] == one["id"], (reshaped, again)
    combed, _ = c.call("comb_guides", dict(f, count=40, part="none", sweep={"front": "back", "sides": "down"}))
    assert combed["guides"] > 10 and combed["shell"]["from"] == "photos", combed
    gone, _ = c.call("guide_op", dict(f, op="delete_guides", group="combed"))
    assert gone["count"] == combed["guides"], gone

    # A root tube drawn round the crown, cut into cells, then split a level down.
    drawn, _ = c.call("root_tube", dict(g, op="draw", center="crown", radius=3, length=12))
    cells, _ = c.call("root_tube", dict(g, op="subdivide", guides=drawn["guides"]))
    assert len(cells["guides"]) >= 3, cells
    split, _ = c.call("tube_levels", dict(g, op="add", group="rootTubes"))
    assert split["split"] > 0 and len(split["levels"]) >= 2, split
    c.call("tube_levels", dict(g, op="view", group="rootTubes", level=len(split["levels"])))
    info, result = c.call("render_preview", dict(g, views=["top"], tubes=True, size=128, look="clay"))
    assert "tubesDrawn" in info and info["look"] == "clay", info

    # An expression checked, then set on a mappable param.
    checked, _ = c.call("check_expression", dict(g, expression="smoothstep($y, 5, 12)"))
    assert checked["ok"] and checked["variables"] == ["y"] and checked["values"]["max"] <= 1.0, checked
    failed, _ = c.call("check_expression", dict(g, expression="1 - $t ^"))
    assert not failed["ok"] and "position" in failed["error"], failed
    graph, _ = c.call("get_graph", g)
    width_node = next(n["name"] for n in graph["nodes"] if n["type"] == "Width")
    c.call("set_params", dict(g, node=width_node, params={"width": {"expression": "1 - $t", "min": 0.001, "max": 0.01}}))

    # A head turn as an animated scalp, the guides simulated through it and baked.
    turned, _ = c.call("animate_scalp", dict(g, keys=[{"frame": 1}, {"frame": 5, "rotate_degrees": [0, 45, 0]},
                                                      {"frame": 9, "rotate_degrees": [0, -20, 0]}]))
    assert turned["frames"] == 9 and os.path.isfile(os.path.join(root, turned["path"])), turned
    sim, _ = c.call("simulate", dict(g, add=True, to_frame=6))
    assert sim["framesStepped"] == 6 and sim["summary"]["errors"] == [], sim
    stats = sim["simulation"][0]
    assert stats["guides"] > 0 and stats["stretch"]["max"] < 0.2 and stats["tipMoveFromGroom"]["max"] > 0, stats
    baked, _ = c.call("sim_cache", dict(g, op="write", last=6))
    assert baked["frames"] == 6 and os.path.isfile(os.path.join(root, baked["path"])), baked

    # A turntable in another look; the cards and the game export.
    sheet, result = c.call("render_preview", dict(g, turntable=8, size=256, look="rim"))
    assert len(sheet["views"]) == 8 and sheet["columns"] == 4 and image_of(result)[:2] == (256, 128), sheet
    c.call("add_node", {"groom": "model", "type": "Cards"})
    cards, _ = c.call("export_cards", {"groom": "model", "path": "out/model.glb", "resolution": 256})
    assert cards["cards"] > 0 and os.path.isfile(os.path.join(root, "out", "model.glb")), cards
    game, _ = c.call("export_game", {"groom": "model", "engine": "unreal", "resolution": 256})
    assert any(f.endswith("_cards.fbx") for f in game["files"]), game
    numbers, _ = c.call("stats", {"groom": "model"})
    assert numbers["stats"]["strands"] == numbers["strands"] > 0 and "cards" in numbers["stats"]["times"], numbers

    # Saved and opened again: the photo lined up, the bake named.
    c.call("save_groom", dict(g, path="out/art.groom"))
    with open(os.path.join(root, "out", "art.groom"), encoding="utf-8") as f:
        text = json.load(f)
    assert text["referencePhotos"][0]["camera"] and text["simCaches"], list(text)
    c.call("open_groom", {"path": "out/art.groom", "name": "art2"})
    again, _ = c.call("photo_view", {"groom": "art2"})
    assert again["linedUp"] and again["flowArrows"] == 1, again


def main():
    exe, root = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    c = Client(exe, root)
    try:
        # ---- the handshake ----
        init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                        "clientInfo": {"name": "test", "version": "1"}})["result"]
        assert init["protocolVersion"] == "2025-06-18", init
        assert "tools" in init["capabilities"] and init["serverInfo"]["name"] == "groomlab", init
        c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        assert c.request("ping")["result"] == {}
        tools = c.request("tools/list")["result"]["tools"]
        names = {t["name"] for t in tools}
        for t in tools:
            assert t["description"] and t["inputSchema"]["type"] == "object" and isinstance(t["inputSchema"]["properties"], dict), t
        for wanted in ["list_nodes", "describe_node", "list_presets", "list_sample_meshes", "get_docs", "new_groom", "open_groom",
                       "apply_preset", "get_graph", "add_node", "remove_node", "connect", "disconnect", "set_params", "add_guides",
                       "set_tube", "add_script_node", "save_node", "undo", "evaluate", "render_preview", "save_groom", "export",
                       "redo", "history", "scalp_landmarks", "brush_stroke", "select_guides", "guide_op", "paint_map",
                       "measure_guides", "add_photo", "photo_view", "mesh_landmarks", "align_photo", "photo_edit",
                       "guides_from_photos", "compare_to_photo", "checkpoint", "restore", "batch", "show_map", "suggest_pairs", "root_tube", "tube_levels", "tube_shape", "check_expression", "import_animation",
                       "animate_scalp", "simulate", "sim_cache", "export_cards", "export_game", "stats", "guides_from_strokes", "comb_guides",
                       "hair_shell", "photo_project", "reshape_guides"]:
            assert wanted in names, wanted
        assert c.request("tools/call", {"name": "no_such_tool", "arguments": {}})["error"]["code"] == -32602
        assert c.request("no/such/method")["error"]["code"] == -32601

        # ---- discover ----
        nodes, _ = c.call("list_nodes")
        assert nodes["count"] > 40, nodes["count"]
        assert any(n["type"] == "node:groomlab.wave" for g in nodes["categories"] for n in g["nodes"]), "library nodes listed"
        curl, _ = c.call("describe_node", {"type": "Curl"})
        assert curl["params"], curl
        presets, _ = c.call("list_presets")
        assert any(p["name"] == "hair_basics" for p in presets["presets"])
        meshes, _ = c.call("list_sample_meshes")
        assert any(m["name"] == "nika" for m in meshes["meshes"]), meshes
        # Buddy: its body (buddy.obj), not the hosts' rigs (buddy_rig.ma, buddy_rig.fbx).
        buddy = [m for m in meshes["meshes"] if m["name"] == "buddy"]
        assert len(buddy) == 1 and buddy[0]["file"] == "buddy.obj" and "rigged" in buddy[0]["description"], meshes
        docs, _ = c.call("get_docs", {"topic": "tube guides"})
        assert docs["text"], docs
        c.call("list_nodes", {"bogus": 1}, expect_error=True)  # an unknown argument says so

        # ---- build ----
        made, _ = c.call("new_groom", {"name": "nika", "mesh": "nika"})
        assert made["summary"]["strands"] > 0 and made["summary"]["errors"] == [], made
        applied, _ = c.call("apply_preset", {"preset": "hair_basics"})
        assert applied["summary"]["strands"] > 0, applied
        graph, _ = c.call("get_graph")
        color = next(n["name"] for n in graph["nodes"] if n["type"] == "HairColor")
        added, _ = c.call("add_node", {"type": "Curl", "after": color})
        curl_node = added["node"]
        param = next(p for p in curl["params"] if p["kind"] == "float" and not p.get("advanced") and "default" in p)
        value = param["default"] * 0.5 if param["default"] else 0.1
        if "max" in param:
            value = min(value, param["max"])
        set_, _ = c.call("set_params", {"node": curl_node, "params": {param["name"]: value}})
        assert set_["set"] == [param["name"]], set_
        c.call("set_params", {"node": curl_node, "params": {"noSuchParam": 1}}, expect_error=True)

        roots, _ = c.call("sample_scalp", {"count": 6, "min_up": 0.5})
        assert len(roots["points"]) == 6, roots
        guides = [{"root": p["p"], "length": 12, "droop": 0.6} for p in roots["points"][:2]]
        top = roots["points"][2]["p"]
        guides.append({"points": [top, [top[0], top[1] + 4, top[2] + 2], [top[0], top[1] + 6, top[2] + 8]]})
        added_guides, _ = c.call("add_guides", {"guides": guides})
        assert len(added_guides["ids"]) == 3 and added_guides["summary"]["guides"] >= 3, added_guides
        tubes, _ = c.call("set_tube", {"guides": added_guides["ids"], "shape": "ribbon", "width": [0.6, 0.2],
                                       "aspect": 0.3, "twist_degrees": 30})
        assert tubes["guides"] == 3, tubes
        listed, _ = c.call("get_guides")
        assert all(g["tube"]["shape"] == "ribbon" for g in listed["guides"]), listed

        broken, _ = c.call("add_script_node", {"label": "Lift", "script": "per point {\n    P.y += $amount * ;\n}",
                                               "params": [{"name": "amount", "type": "length", "default": 0.5}]},
                           expect_error=True)
        assert broken["errors"] and broken["errors"][0]["line"] == 2, broken
        fixed, _ = c.call("add_script_node", {"label": "Lift", "script": "per point {\n    P.y += $amount * t;\n}",
                                              "params": [{"name": "amount", "type": "length", "default": 0.5}],
                                              "after": curl_node})
        script_node = fixed["node"]
        assert fixed["summary"]["errors"] == [], fixed

        # ---- check ----
        evaluated, _ = c.call("evaluate")
        assert evaluated["errors"] == [] and evaluated["strands"] > 0 and "nodeMs" in evaluated, evaluated
        strands = evaluated["strands"]

        # ---- see ----
        info, result = c.call("render_preview", {"views": ["front", "three_quarter"], "size": 256, "save_to": "out/preview.png"})
        image = next(x for x in result["content"] if x["type"] == "image")
        assert image["mimeType"] == "image/png"
        png = base64.b64decode(image["data"])
        width, height, rows = png_pixels(png)
        assert (width, height) == (256, 128), (width, height)
        colors = {row[i:i + 3] for row in rows for i in range(0, len(row), 3)}
        assert len(colors) > 50, "the preview draws something (%d colors)" % len(colors)
        assert os.path.getsize(os.path.join(root, "out", "preview.png")) == len(png)
        _, again = c.call("render_preview", {"views": ["front", "three_quarter"], "size": 256})
        assert next(x for x in again["content"] if x["type"] == "image")["data"] == image["data"], "the same preview twice"

        # ---- deliver ----
        node_file, _ = c.call("save_node", {"node": script_node, "path": "out/lift.glnode"})
        assert os.path.isfile(os.path.join(root, "out", "lift.glnode")), node_file
        saved, _ = c.call("save_groom", {"path": "out/nika.groom"})
        assert os.path.isfile(os.path.join(root, "out", "nika.groom")), saved
        opened, _ = c.call("open_groom", {"path": "out/nika.groom", "name": "nika2"})
        assert opened["summary"]["strands"] == strands and opened["summary"]["errors"] == [], opened
        exported, _ = c.call("export", {"groom": "nika2", "path": "out/nika.usda", "frames": "1"})
        with open(os.path.join(root, "out", "nika.usda"), encoding="utf-8") as f:
            assert "BasisCurves" in f.read(), exported
        assert exported["strandsPerFrame"] == strands, exported

        # ---- safety ----
        outside, _ = c.call("save_groom", {"groom": "nika", "path": "../outside.groom"}, expect_error=True)
        assert "outside" in outside["error"], outside
        assert not os.path.exists(os.path.join(os.path.dirname(root), "outside.groom"))
        c.call("open_groom", {"path": os.path.join(os.path.dirname(root), "x.groom")}, expect_error=True)
        c.call("new_groom", {"name": "bad", "mesh": "../../etc/scalp.obj"}, expect_error=True)
        c.call("evaluate", {}, expect_error=True)  # two grooms open: say which

        # ---- undo ----
        undone, _ = c.call("undo", {"groom": "nika"})
        assert undone["undone"] == ["add script node"], undone
        graph, _ = c.call("get_graph", {"groom": "nika"})
        assert all(n["name"] != script_node for n in graph["nodes"]), "the script node is gone"

        # ---- hand-placed feathers: the preset's guide group made, each guide added to it a feather ----
        bird, _ = c.call("apply_preset", {"groom": "nika2", "preset": "hand_placed_feathers"})
        assert bird["groupsMade"] == ["feathers"] and bird["summary"]["errors"] == [], bird
        assert bird["summary"]["strands"] == 0, bird
        drawn, _ = c.call("add_guides", {"groom": "nika2", "group": "feathers", "guides": guides[:2]})
        assert drawn["summary"]["strands"] == 2 * (1 + 140), drawn

        groom_by_hand(c)
        everything_else(c, root)
    finally:
        code = c.close()
    assert code == 0, "the server exited with %d" % code
    print("mcp session: ok")


if __name__ == "__main__":
    main()
