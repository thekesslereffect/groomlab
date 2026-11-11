# SPDX-License-Identifier: GPL-3.0-or-later
"""Param units in Blender's UI. The groom keeps every length in centimeters (as Maya and .groom
files do, docs/BLENDER.md); the panels show lengths and widths in scene units (Blender's own
length display: meters, millimeters... per the scene's unit settings), per-length rates per scene
unit, densities per square centimeter (hairs per cm², what hair density is quoted in), and the
spec's other units with a fixed suffix."""

import json

SCENE_LENGTH = {"length", "width"}
SUFFIX = {"density": "/cm²", "angle": "°", "turns": " turns", "seconds": " s", "pixels": " px"}


def meters_per_unit(scene):
    scale = float(scene.unit_settings.scale_length) if scene is not None else 1.0
    return scale if scale > 0.0 else 1.0


def cm_per_unit(scene):
    return 100.0 * meters_per_unit(scene)


def condition_text(value):
    """A param's value as a showWhen or unitWhen compares it: the core's rule (param_conditions.h:
    conditionText), which the add-on cannot call: a bool "true"/"false", a text as it is, a whole
    number as an integer ("3", also when stored as 3.0), any other number as JSON writes it."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return value
    if isinstance(value, float) and value.is_integer() and abs(value) < 9.0e15:
        return str(int(value))
    return json.dumps(value)


def condition_holds(condition, value):
    """Whether a condition ({"param", "in"}: showWhen, unitWhen) holds for its param's `value` (the
    core's conditionHolds). No condition: it holds."""
    if not isinstance(condition, dict) or not condition.get("param"):
        return True
    return condition_text(value) in (condition.get("in") or [])


def effective(spec, value_of):
    """What a param measures for its node now: its spec's unit while its unitWhen holds (graph.h
    ParamSpec::unitWhen: Noise's frequency is per scene unit in world space, per strand length in
    strand space), else "" (unitless). `value_of(param)`: the node's value of another param."""
    unit = spec.get("unit") or ""
    when = spec.get("unitWhen")
    if unit and when and not condition_holds(when, value_of(when.get("param"))):
        return ""
    return unit


def to_ui(unit, value, scene):
    """A param's stored value (centimeters) as the panel shows it."""
    if unit in SCENE_LENGTH:
        return value / cm_per_unit(scene)
    if unit == "perLength":
        return value * cm_per_unit(scene)
    return value


def from_ui(unit, value, scene):
    if unit in SCENE_LENGTH:
        return value * cm_per_unit(scene)
    if unit == "perLength":
        return value / cm_per_unit(scene)
    return value


def suffix(unit, scene):
    if unit == "perLength":
        metric = scene is not None and scene.unit_settings.system != "NONE" and abs(meters_per_unit(scene) - 1.0) < 1e-9
        return " /m" if metric else " /unit"
    return SUFFIX.get(unit, "")
