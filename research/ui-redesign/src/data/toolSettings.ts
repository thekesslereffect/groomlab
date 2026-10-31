// SPDX-License-Identifier: Apache-2.0
// What each tool's settings card holds, from engine/core/src/brush_catalog.cpp and the Maya Groom
// Tools (hosts/maya/scripts/groomlab/ui/palette.py show_tool_settings) as of 2026-10-30.

export type ToolSettings = {
  brush?: boolean; // Radius, Strength, Falloff
  signed?: boolean; // Strength is -1..1 (Ctrl inverts)
  smooth?: boolean; // Paint's Smooth strength
  falloffCurve?: boolean;
  rootToTip?: boolean;
  symmetry?: boolean;
  options?: ("selectedOnly" | "pressure" | "collide" | "frontFacing" | "stretch")[];
  actsOn?: boolean; // Guides / Parts / Flow
  erases?: boolean;
  newGuides?: "place" | "draw";
  tube?: "new" | "edit"; // the tube shape new guides take / edits to the selected guides
  paints?: boolean; // Shape: Width / Flatness / Twist
  parts?: boolean;
  flow?: boolean;
  lengthKeys?: boolean;
  softRadius?: boolean;
  alpha?: boolean;
  short: string; // the one line under the tool's name
};

const shaping: ToolSettings["options"] = ["selectedOnly", "pressure", "collide", "frontFacing"];

export const toolSettings: Record<string, ToolSettings> = {
  place: { symmetry: true, options: ["collide"], newGuides: "place", tube: "new", short: "Click the scalp to plant a guide; drag to lay down many." },
  draw: { symmetry: true, options: ["collide"], newGuides: "draw", tube: "new", short: "Press on the scalp and draw a guide's shape." },
  part: { symmetry: true, parts: true, short: "Draw a line the strands split along." },
  flow: { brush: true, falloffCurve: true, options: ["pressure", "frontFacing"], flow: true, short: "Paint the direction the fur lies." },
  comb: { brush: true, falloffCurve: true, rootToTip: true, symmetry: true, options: [...shaping, "stretch"], actsOn: true, short: "Drag guides along the stroke, roots pinned." },
  slide: { brush: true, falloffCurve: true, symmetry: true, options: shaping, short: "Slide guides over the scalp." },
  smooth: { brush: true, falloffCurve: true, rootToTip: true, symmetry: true, options: shaping, actsOn: true, short: "Even out kinks." },
  straighten: { brush: true, falloffCurve: true, rootToTip: true, symmetry: true, options: shaping, actsOn: true, short: "Pull guides toward straight." },
  lift: { brush: true, signed: true, falloffCurve: true, rootToTip: true, symmetry: true, options: shaping, short: "Raise guides off the scalp (Ctrl: lay down)." },
  twist: { brush: true, signed: true, falloffCurve: true, rootToTip: true, symmetry: true, options: shaping, actsOn: true, short: "Twist guides around their root." },
  attract: { brush: true, signed: true, falloffCurve: true, rootToTip: true, symmetry: true, options: shaping, actsOn: true, short: "Pull guide tips together." },
  shape: { brush: true, signed: true, falloffCurve: true, rootToTip: true, symmetry: true, options: ["selectedOnly", "pressure", "frontFacing"], paints: true, tube: "edit", short: "Grow or shrink the tube (Shift: smooth)." },
  cut: { brush: true, falloffCurve: true, symmetry: true, options: shaping, actsOn: true, short: "Trim guides where the brush touches them." },
  length: { brush: true, signed: true, falloffCurve: true, symmetry: true, options: shaping, actsOn: true, short: "Grow guides evenly (Ctrl: shorten)." },
  lengths: { lengthKeys: true, short: "Keys on the scalp set guide lengths; click to add one." },
  erase: { brush: true, falloffCurve: true, symmetry: true, options: shaping, erases: true, short: "Remove guides, parts or flow under the brush." },
  paint: { brush: true, smooth: true, falloffCurve: true, symmetry: true, options: ["selectedOnly", "pressure", "frontFacing"], alpha: true, short: "Paint the current map (Shift: smooth, Ctrl: toward 0)." },
  select: { softRadius: true, tube: "edit", short: "Click or box guides; the rest freeze." },
  freeze: { brush: true, falloffCurve: true, options: ["selectedOnly", "pressure", "frontFacing"], short: "Protect guides from brushes." },
};

export const optionLabels: Record<string, { label: string; tip: string; on: boolean }> = {
  selectedOnly: { label: "Selected only", tip: "Only the selected guides", on: false },
  pressure: { label: "Pressure", tip: "Pen pressure scales the strength", on: true },
  collide: { label: "Off scalp", tip: "Keep guides off the scalp", on: true },
  frontFacing: { label: "Front only", tip: "Only the side of the scalp facing the camera", on: true },
  stretch: { label: "Stretch", tip: "Guides get longer or shorter as you pull", on: false },
};

export const gestures: [string, string][] = [
  ["B + drag", "Radius"],
  ["M + drag", "Strength"],
  ["Ctrl", "The opposite (shorten, lay down, toward 0)"],
  ["Shift", "Smooth instead (painting, Shape)"],
  ["Shift + drag", "Paint guides into the selection"],
  ["Page Up / Down", "Brush level (parents / children)"],
  ["Alt+Shift+X", "Symmetry on / off"],
  ["Y + click", "Marking menu: every brush"],
];

export const tubeShapes = ["Round", "Flat ribbon", "Oval", "Braid strand", "Custom: lock"];
