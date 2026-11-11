# SPDX-License-Identifier: Apache-2.0
"""Real-world units for GroomLab's UI.

Groom data is in scene units, which are centimeters: Maya's API (and so the
scalp, guides and strands GroomLab reads) always works in centimeters,
whatever the UI's linear unit. Params that measure something say so in their
spec ("unit": "length", "width" or "density"), and the UI shows them in the
units picked here, stored per user. The spec's other units show with a fixed
suffix: a fraction as a percentage (x100), angles in degrees, turns, seconds,
pixels, and a per-length rate per the length unit picked:

    from groomlab import units
    units.set_unit("width", "um")      # widths in micrometers (hair: ~50-120 um)
    units.to_display("width", 0.006)   # -> 60.0
"""

from maya import cmds

from groomlab.ui import definitions

# Every host's units (engine/ui/choices.json: unitLength, unitWidth, unitDensity, unitFixed,
# unitPerTime), read once here as this module has always held them.
_PICKED = {"length": "unitLength", "width": "unitWidth", "density": "unitDensity"}
# kind -> [(key, label, display units per scene unit)]
UNITS = {kind: [(c["value"], c["label"], float(c["factor"])) for c in definitions.choices(name)]
         for kind, name in _PICKED.items()}
DEFAULTS = {kind: definitions.choice_default(name) for kind, name in _PICKED.items()}
# What real hair measures, in scene units: sliders of that kind center on it.
TYPICAL = {"density": 200.0}  # hairs per cm² on a human scalp (roughly 150-300)
# The spec's units no one picks (graph.h ParamSpec::unit): kind -> (key, label, factor).
FIXED = {c["value"]: (c["value"], c["label"], float(c["factor"])) for c in definitions.choices("unitFixed")}
# Params whose number is a rate of their unit over time (the spec's unit measures the length part):
# (node type, param) -> what follows the unit. Simulate's gravity is 980 cm/s², not 980 cm.
PER_TIME = {tuple(c["value"].split(".", 1)): c["label"] for c in definitions.choices("unitPerTime")}


def param_label(kind, op_type=None, param=None):
    """The unit shown after a param's field: label(kind), and "/s²" for an acceleration (PER_TIME)."""
    text = label(kind)
    return text + PER_TIME.get((op_type, param), "") if text else text


def _var(kind):
    return "groomLabUnit_" + kind


def unit(kind):
    """(key, label, factor) for a kind: display value = scene value x factor. A per-length rate
    ("perLength") follows the length unit: per mm is a tenth of per cm."""
    if kind in FIXED:
        return FIXED[kind]
    if kind == "perLength":
        key, text, f = unit("length")
        return ("per_" + key, "/" + ("unit" if key == "scene" else text), 1.0 / f)
    choices = UNITS[kind]
    key = cmds.optionVar(query=_var(kind)) if cmds.optionVar(exists=_var(kind)) else DEFAULTS[kind]
    return next((c for c in choices if c[0] == key), next(c for c in choices if c[0] == DEFAULTS[kind]))


def set_unit(kind, key):
    if key not in [c[0] for c in UNITS[kind]]:
        raise ValueError("no %s unit %r (%s)" % (kind, key, ", ".join(c[0] for c in UNITS[kind])))
    cmds.optionVar(stringValue=(_var(kind), key))


def shown(kind):
    """Whether the UI shows a kind's values with a unit (else unitless, or a color)."""
    return kind in UNITS or kind in FIXED or kind == "perLength"


def factor(kind):
    return unit(kind)[2] if shown(kind) else 1.0


def label(kind):
    return unit(kind)[1] if shown(kind) else ""


def soft_range(kind):
    """A slider range centered on the typical value of a kind, in display units; None when the
    kind has no typical value (the range then comes from the param's default)."""
    if kind not in TYPICAL:
        return None
    return 0.0, 2.0 * TYPICAL[kind] * factor(kind)


def to_display(kind, value):
    return value * factor(kind)


def from_display(kind, value):
    return value / factor(kind)
