# SPDX-License-Identifier: Apache-2.0
"""Check authored icon assets and reproducible SVG/PNG generation.

    python tests/engine/ui/check_icons.py

Requires Pillow and resvg-py, as tools/make_blender_icons.py does.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from make_icons import DESIGNED_ICONS


def main():
    for name in DESIGNED_ICONS:
        file = "groomLab_%s.svg" % name
        svg = ROOT / "hosts/maya/icons" / file
        assert svg.read_bytes() == (ROOT / "hosts/app/icons" / file).read_bytes(), name
        root = ET.parse(svg).getroot()
        assert root.get("viewBox") == "0 0 100 100", name
        assert not root.findall(".//{*}mask"), name
        paths = root.findall(".//{*}path")
        assert paths and all(path.get("fill") and not path.get("stroke") for path in paths), name
        assert (ROOT / "hosts/blender/addon/groomlab/icons" / (name + ".png")).is_file(), name

    # Regenerate in a temporary checkout, keeping the working tree untouched.
    with tempfile.TemporaryDirectory(prefix="groomlab-icons-") as directory:
        target = Path(directory)
        for folder in ("tools", "hosts/maya/icons", "hosts/maya/themes", "hosts/app/icons",
                       "hosts/blender/addon/groomlab/icons"):
            shutil.copytree(ROOT / folder, target / folder)
        paths = [path for folder in ("hosts/maya/icons", "hosts/app/icons", "hosts/blender/addon/groomlab/icons")
                 for path in (target / folder).iterdir() if path.suffix in (".svg", ".png")]
        before = {path: path.read_bytes() for path in paths}
        for script in ("make_icons.py", "make_blender_icons.py"):
            subprocess.run([sys.executable, str(target / "tools" / script)], check=True)
        for path, expected in before.items():
            assert path.read_bytes() == expected, "generation changed %s" % path.relative_to(target)
    print("19 outlined designs match Maya/app assets; all SVGs and Blender PNGs reproduce")


if __name__ == "__main__":
    main()
