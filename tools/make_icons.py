# SPDX-License-Identifier: Apache-2.0
"""Writes GroomLab's icons (hosts/maya/icons/groomLab_*.svg): python tools/make_icons.py

Flat, chunky and round. Colors are placeholders the theme replaces when an
icon loads (groomlab.ui.widgets.icon):
  #3D7EFF main   the tool group's color      #1F4FB3 shade   a darker main
  #A8C7FF tint   a lighter main              #F2F2F5 ink     hair and neutral parts
  #FFFFFF shine  highlights                  #FF4D50 #FFC53E #2F7BF7 #2ECC90: palette red, yellow, blue, emerald
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "hosts", "maya", "icons")
M, S, T, K, W = "#3D7EFF", "#1F4FB3", "#A8C7FF", "#F2F2F5", "#FFFFFF"
RED, YEL, BLU, EME = "#FF4D50", "#FFC53E", "#2F7BF7", "#2ECC90"


def line(d, color=K, w=2.6, extra=""):
    return '<path d="%s" stroke="%s" stroke-width="%s"%s/>' % (d, color, w, extra)


def fill(d, color=M, extra=""):
    return '<path d="%s" fill="%s"%s/>' % (d, color, extra)


ICONS = {
    "place": [
        line("M4 29 H17", S, 3),
        line("M10 29 C10 22 13 17 10.5 9"),
        '<circle cx="22" cy="10" r="7.5" fill="%s"/>' % M,
        '<circle cx="19.2" cy="6.9" r="1.6" fill="%s" opacity=".55"/>' % W,
        line("M22 6.4 V13.6 M18.4 10 H25.6", W, 2.6),
    ],
    "draw": [
        line("M4 27 C8 21 11 27 15 22", K, 2.6),
        '<g transform="rotate(45 21 11)">'
        '<rect x="17" y="1.5" width="8" height="17" rx="3.2" fill="%s"/>'
        '<rect x="17" y="1.5" width="8" height="4.5" rx="2.2" fill="%s"/>'
        '<path d="M17.4 18 L21 24.5 L24.6 18 Z" fill="%s"/>'
        '<path d="M20 22.6 L21 24.5 L22 22.6 Z" fill="%s"/>'
        '<rect x="18.6" y="7.5" width="1.6" height="8" rx=".8" fill="%s" opacity=".5"/></g>' % (M, T, K, S, W),
    ],
    "part": [
        line("M16 29 V4", M, 2.8, ' stroke-dasharray="0.1 4.6"'),
        line("M13 29 C13 19 9 13 4 10"),
        line("M19 29 C19 19 23 13 28 10"),
        line("M9.5 29 C9.5 22 7 18 3 16.5", K, 2.2, ' opacity=".55"'),
        line("M22.5 29 C22.5 22 25 18 29 16.5", K, 2.2, ' opacity=".55"'),
    ],
    "flow": [
        line("M4 24 C9 24 11 18 17 18 C21 18 23 20 27 20", M, 2.8),
        line("M23.6 16.6 L27.4 20 L23.6 23.4", M, 2.8),
        line("M4 12 C9 12 11 6 17 6 C21 6 23 8 27 8", T, 2.8),
        line("M23.6 4.6 L27.4 8 L23.6 11.4", T, 2.8),
        '<circle cx="4.5" cy="24" r="2.4" fill="%s"/><circle cx="4.5" cy="12" r="2.4" fill="%s"/>' % (S, S),
    ],
    "select": [
        '<ellipse cx="14" cy="11" rx="10.5" ry="7" stroke="%s" stroke-width="2.6" stroke-dasharray="3.4 3.4"/>' % M,
        line("M8.5 16.5 C7 20.5 11 22 9.5 27", M, 2.6),
        fill("M17 13 L27.5 21.5 L22.8 22.4 L25.4 27.6 L22.6 28.9 L20 23.7 L17 27 Z", K,
             ' stroke="%s" stroke-width="1.4" stroke-linejoin="round"' % S),
    ],
    "comb": [
        line("M9 29 C9 24 10 21 9 18"), line("M16 29 C16 24 17 21 16 18"), line("M23 29 C23 24 24 21 23 18"),
        line("M8 11 V17 M12 11 V16 M16 11 V17 M20 11 V16 M24 11 V17", M, 2.6),
        '<rect x="4" y="4" width="24" height="8" rx="4" fill="%s"/>' % M,
        '<rect x="7.5" y="6" width="9" height="2" rx="1" fill="%s" opacity=".55"/>' % W,
    ],
    "slide": [
        line("M3 27 Q16 20 29 27", S, 3),
        line("M16 23 C16 16 19 12 17 5"),
        '<circle cx="16" cy="23" r="3.2" fill="%s"/>' % M,
        line("M8.5 18.5 L5 22 L9 24.5 M23.5 18.5 L27 22 L23 24.5", M, 2.6),
    ],
    "smooth": [
        line("M6 29 L10 24 L6.5 19.5 L10.5 14.5 L7 10 L9 5", T, 2.4),
        line("M14 16 H18 M16.2 13.6 L18.6 16 L16.2 18.4", M, 2.6),
        line("M23 29 C23 21 26 15 24 5"),
    ],
    "lift": [
        line("M3 28 H29", S, 3),
        line("M8 28 C14 27.5 20 26.5 27 24.5", T, 2.4, ' stroke-dasharray="2.5 3.5"'),
        line("M8 28 C8 21 9 15 8.5 7"),
        line("M25 20 A15 15 0 0 0 15 6.5", M, 2.8),
        line("M15.6 2.8 L14.6 6.8 L18.6 8.4", M, 2.8),
    ],
    "twist": [
        line("M16 26 C17.5 19 20.5 13 25 6"),
        '<path d="M7.5 25 A8.5 3.6 0 1 0 16 21.4" stroke="%s" stroke-width="2.8"/>' % M,
        line("M12.3 19.2 L16.3 21.4 L12.6 24.4", M, 2.6),
    ],
    "attract": [
        '<path d="M8 4 V14 A8 8 0 0 0 24 14 V4" stroke="%s" stroke-width="6.5"/>' % M,
        line("M8 4 V7.5 M24 4 V7.5", W, 6.5, ' stroke-linecap="butt"'),
        line("M9 30 L16 25.5 L23 30", K, 2.4),
    ],
    "cut": [
        line("M11.8 19.2 L24.5 3.5", K, 3),
        line("M19.2 20.8 L7.5 3.5", K, 3),
        '<circle cx="9" cy="23.5" r="4.4" stroke="%s" stroke-width="3"/>' % M,
        '<circle cx="22" cy="24.5" r="4.4" stroke="%s" stroke-width="3"/>' % M,
        '<circle cx="15.6" cy="14.6" r="1.6" fill="%s"/>' % S,
    ],
    "length": [
        line("M10 28 C10 20 12 14 10 5"),
        line("M22 7 V25 M18 10.8 L22 7 L26 10.8 M18 21.2 L22 25 L26 21.2", M, 2.8),
    ],
    "lengths": [
        line("M7 24 C7 20.5 8 18 7 15 M16 24 C16 18.5 17 14 16 9 M25 24 C25 16.5 26 10 25 4"),
        line("M4 27.5 H28", M, 2.6),
        '<circle cx="7" cy="27.5" r="2.8" fill="%s"/>' % M,
        '<circle cx="25" cy="27.5" r="2.8" fill="%s"/>' % M,
    ],
    "shape": [
        '<path d="M8 8 V24 A8 3.2 0 0 0 24 24 V8 A8 3.2 0 0 1 8 8 Z" fill="%s" opacity=".22"/>' % M,
        line("M16 30 C16 23 17 16 16 3", K, 2.4),
        '<ellipse cx="16" cy="8" rx="8" ry="3.2" stroke="%s" stroke-width="2.6"/>' % M,
        '<path d="M8 8 V24 A8 3.2 0 0 0 24 24 V8" stroke="%s" stroke-width="2.6"/>' % M,
    ],
    "erase": [
        line("M4 28 H28", K, 2.4, ' stroke-dasharray="0.1 4.2"'),
        '<g transform="rotate(-35 16 15)">'
        '<rect x="6" y="9" width="20" height="12" rx="3.5" fill="%s"/>'
        '<rect x="6" y="9" width="8" height="12" rx="3.5" fill="%s"/>'
        '<rect x="16" y="11" width="7" height="2" rx="1" fill="%s" opacity=".5"/></g>' % (M, T, W),
    ],
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
    "freeze": [
        line("M16 3 V29 M4.7 9.5 L27.3 22.5 M4.7 22.5 L27.3 9.5", M, 2.8),
        line("M12.5 5 L16 8.2 L19.5 5 M12.5 27 L16 23.8 L19.5 27", M, 2.4),
        line("M4.4 14.2 L8.8 12.7 L7.5 8.2 M27.6 17.8 L23.2 19.3 L24.5 23.8", M, 2.4),
        line("M7.5 23.8 L8.8 19.3 L4.4 17.8 M24.5 8.2 L23.2 12.7 L27.6 14.2", M, 2.4),
        '<circle cx="16" cy="16" r="2.6" fill="%s"/>' % W,
    ],
    "unfreeze": [
        line("M16 3 V29 M4.7 9.5 L27.3 22.5 M4.7 22.5 L27.3 9.5", T, 2.8),
        line("M12.5 5 L16 8.2 L19.5 5 M12.5 27 L16 23.8 L19.5 27", T, 2.4),
        '<circle cx="16" cy="16" r="2.6" fill="%s"/>' % W,
        line("M5 27 L27 5", RED, 3.2),
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
for name, parts in ICONS.items():
    with open(os.path.join(OUT, "groomLab_%s.svg" % name), "w", newline="\n") as f:
        f.write(HEAD + "".join(parts) + "</svg>\n")
print(len(ICONS), "icons")
