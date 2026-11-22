# SPDX-License-Identifier: Apache-2.0
"""Where the files every host ships live (presets, nodes, meshes, alphas, themes, expressions): the module's
own folder when installed (<module>/<kind>), else a source checkout's engine/assets/<kind>."""

import os

_HERE = os.path.dirname(os.path.abspath(__file__))


def shipped_dir(kind):
    """The folder of shipped `kind` files (presets, nodes, meshes, alphas, themes or expressions)."""
    installed = os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, kind))
    source = os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir, "engine", "assets", kind))
    return installed if os.path.isdir(installed) or not os.path.isdir(source) else source
