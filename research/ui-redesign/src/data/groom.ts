// SPDX-License-Identifier: Apache-2.0
// Mock content, taken from the real definitions: engine/core/src/brush_catalog.cpp (tools),
// engine/ui/properties.json (sections), hosts/maya/themes/charcoal.json (graph colors).

export type Tool = { id: string; label: string; icon: string; tip: string };
export type ToolGroup = { id: string; label: string; tools: Tool[] };

import { icon } from "./icons";

const t = (id: string, label: string, tip: string, name = id): Tool => ({ id, label, icon: icon(`groomLab_${name}`), tip });

export const toolGroups: ToolGroup[] = [
  {
    id: "create", label: "Create", tools: [
      t("place", "Place", "Click the scalp to plant a guide; drag to lay down many"),
      t("draw", "Draw", "Press on the scalp and draw a guide's shape"),
      t("part", "Part", "Draw a part line the strands split along"),
      t("flow", "Flow", "Paint the direction the fur lies"),
    ],
  },
  {
    id: "shape", label: "Shape", tools: [
      t("comb", "Comb", "Drag guides along the stroke"),
      t("slide", "Slide", "Slide guides over the scalp"),
      t("smooth", "Smooth", "Even out kinks"),
      t("straighten", "Straighten", "Pull guides toward straight"),
      t("lift", "Lift", "Raise guides off the scalp (Ctrl: lay down)"),
      t("twist", "Twist", "Twist guides around their root"),
      t("attract", "Attract", "Pull guide tips together"),
      t("shape", "Shape", "Grow or shrink the tube (Shift: smooth)"),
    ],
  },
  {
    id: "trim", label: "Trim", tools: [
      t("cut", "Cut", "Trim guides where the brush touches them"),
      t("length", "Length", "Grow guides evenly (Ctrl: shorten)"),
      t("lengths", "Length Keys", "Keys on the scalp set guide lengths"),
      t("erase", "Erase", "Remove guides"),
    ],
  },
  {
    id: "guides", label: "Guides", tools: [
      t("select", "Select", "Click or box guides", "select"),
      t("freeze", "Freeze", "Protect guides from brushes"),
      t("unfreeze", "Unfreeze All", "Unfreeze every guide"),
    ],
  },
];

export const allTools = toolGroups.flatMap((g) => g.tools);

export const guideGroups = [
  { name: "primary", color: "#7C9CD4", guides: 64, visible: true },
  { name: "bangs", color: "#D4B26A", guides: 22, visible: true },
  { name: "nape", color: "#6DB897", guides: 18, visible: false },
];

export const graphCategories: Record<string, string> = {
  Sources: "#6DB897", Grow: "#5EB0AA", Shape: "#7C9CD4", Color: "#CF84A8", Look: "#D4B26A",
  Utility: "#8C8C8C", Output: "#E5E5E5", Deform: "#8AA6D6",
};

export type GNode = {
  id: string; label: string; cat: string; x: number; y: number; ins: string[]; outs: string[];
  values?: [string, string][]; ms?: number; view?: boolean; bypass?: boolean; warning?: string;
};

export const graphNodes: GNode[] = [
  { id: "guides", label: "Guides", cat: "Sources", x: 10, y: 20, ins: [], outs: ["guides"], values: [["Guides", "104"]], ms: 0.1 },
  { id: "scatter", label: "Scatter", cat: "Grow", x: 10, y: 150, ins: ["mesh"], outs: ["strands"], values: [["Count", "25.8k"], ["Seed", "1"]], ms: 2.4 },
  { id: "interp", label: "Interpolate", cat: "Grow", x: 190, y: 80, ins: ["strands", "guides"], outs: ["strands"], values: [["Guide Weight", "1.00"]], ms: 11.8 },
  { id: "clump", label: "Clump", cat: "Shape", x: 370, y: 30, ins: ["strands", "guides"], outs: ["strands"], values: [["Strength", "0.80"], ["Clump Size", "0.35"]], ms: 6.1 },
  { id: "noise", label: "Noise", cat: "Shape", x: 550, y: 110, ins: ["strands"], outs: ["strands"], values: [["Amplitude", "0.10"]], ms: 1.9, bypass: true },
  { id: "width", label: "Width", cat: "Look", x: 730, y: 40, ins: ["strands"], outs: ["strands"], values: [["Root", "0.06"], ["Tip", "0.01"]], ms: 0.4, view: true },
  { id: "color", label: "Hair Color", cat: "Color", x: 910, y: 110, ins: ["strands"], outs: ["strands"], values: [["Melanin", "0.70"]], ms: 0.6, warning: "Dye is white: Result follows Melanin only" },
  { id: "output", label: "Output", cat: "Output", x: 1090, y: 60, ins: ["strands"], outs: [] },
];

export const graphWires: [string, number, string, number][] = [
  ["scatter", 0, "interp", 0],
  ["guides", 0, "interp", 1],
  ["interp", 0, "clump", 0],
  ["guides", 0, "clump", 1],
  ["clump", 0, "noise", 0],
  ["noise", 0, "width", 0],
  ["width", 0, "color", 0],
  ["color", 0, "output", 0],
];

export const portColors: Record<string, string> = {
  strands: "#D4B26A", guides: "#7C9CD4", mesh: "#6DB897",
};
