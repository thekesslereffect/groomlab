# SPDX-License-Identifier: Apache-2.0
"""Writes GroomLab's icons (hosts/maya/icons/groomLab_*.svg): python tools/make_icons.py

The 19 tool designs are authored SVGs in hosts/maya/icons, synced to the app.
The remaining icons are generated here. Colors are placeholders the theme replaces when an
icon loads (groomlab.ui.widgets.icon):
  #3D7EFF main   the tool group's color      #1F4FB3 shade   a darker main
  #A8C7FF tint   a lighter main              #F2F2F5 ink     hair and neutral parts
  #FFFFFF shine  highlights                  #FF4D50 #FFC53E #2F7BF7 #2ECC90: palette red, yellow, blue, emerald
"""
import os
import shutil

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "hosts", "maya", "icons")
M, S, T, K, W = "#3D7EFF", "#1F4FB3", "#A8C7FF", "#F2F2F5", "#FFFFFF"
RED, YEL, BLU, EME = "#FF4D50", "#FFC53E", "#2F7BF7", "#2ECC90"


def line(d, color=K, w=2.6, extra=""):
    return '<path d="%s" stroke="%s" stroke-width="%s"%s/>' % (d, color, w, extra)


def fill(d, color=M, extra=""):
    return '<path d="%s" fill="%s"%s/>' % (d, color, extra)


ICONS = {
    "paint": [
        fill("M24.5 2.5 L29.5 7.5 L18 19 L13 14 Z", M),
        fill("M13 14 L18 19 L15.8 21.2 L10.8 16.2 Z", T),
        fill("M10.8 16.2 L15.8 21.2 C14.5 26.5 9 28.5 3.5 28.5 C3.5 23 5.5 17.5 10.8 16.2 Z", S),
        fill("M25 19 C25 19 29 23.5 29 25.8 A4 4 0 0 1 21 25.8 C21 23.5 25 19 25 19 Z", M),
        '<circle cx="23.6" cy="25.2" r="1.1" fill="%s" opacity=".6"/>' % W,
    ],
    "clump": [
        line("M5 30 C7 21 13 16 16 6", K, 2.4),
        line("M16 30 V6", K, 2.4),
        line("M27 30 C25 21 19 16 16 6", K, 2.4),
        '<rect x="9.5" y="13" width="13" height="6" rx="3" fill="%s"/>' % M,
        '<rect x="11.5" y="14.3" width="5" height="1.6" rx=".8" fill="%s" opacity=".6"/>' % W,
    ],
    "graph": [
        line("M13 11 C17 11 16 8 19 8", K, 2.2),
        line("M13 11 C17 11 16 22 19 22", K, 2.2),
        '<rect x="2.5" y="7" width="11" height="8" rx="3.2" fill="%s"/>' % EME,
        '<rect x="18.5" y="4" width="11" height="8" rx="3.2" fill="%s"/>' % BLU,
        '<rect x="18.5" y="18" width="11" height="8" rx="3.2" fill="%s"/>' % YEL,
    ],
    "palette": [
        '<rect x="3" y="3" width="12" height="12" rx="4" fill="%s"/>' % BLU,
        '<rect x="17" y="3" width="12" height="12" rx="4" fill="%s"/>' % YEL,
        '<rect x="3" y="17" width="12" height="12" rx="4" fill="%s"/>' % EME,
        '<rect x="17" y="17" width="12" height="12" rx="4" fill="%s"/>' % RED,
        '<circle cx="6.5" cy="6.5" r="1.4" fill="%s" opacity=".6"/>' % W,
    ],
    "layers": [
        fill("M16 20 L29 13.5 L16 7 L3 13.5 Z", S),
        fill("M16 16 L29 9.5 L16 3 L3 9.5 Z", M),
        line("M3 18.5 L16 25 L29 18.5", T, 2.6),
        line("M3 23 L16 29.5 L29 23", T, 2.6, ' opacity=".6"'),
    ],
}

HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32" fill="none" '
        'stroke-linecap="round" stroke-linejoin="round">')
# These SVGs are authored directly in hosts/maya/icons. Keep their outlined artwork intact.
DESIGNED_ICONS = (
    "place", "draw", "part", "flow", "comb", "slide", "smooth", "lift", "twist", "attract",
    "shape", "cut", "length", "lengths", "erase", "select", "freeze", "invert", "unfreeze",
)


def main():
    for name, parts in ICONS.items():
        with open(os.path.join(OUT, "groomLab_%s.svg" % name), "w", newline="\n") as f:
            f.write(HEAD + "".join(parts) + "</svg>\n")
    app = os.path.join(OUT, "..", "..", "app", "icons")
    for name in DESIGNED_ICONS:
        file = "groomLab_%s.svg" % name
        shutil.copyfile(os.path.join(OUT, file), os.path.join(app, file))
    print(len(ICONS), "generated icons;", len(DESIGNED_ICONS), "authored icons synced to app")


if __name__ == "__main__":
    main()
