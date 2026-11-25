// SPDX-License-Identifier: Apache-2.0
// Content for the Simple (Maya-first) mock, read from the real definitions where they exist:
// engine/core/src/brush_catalog.cpp (the brushes and their groups), engine/ui/actions.json (every
// command, for the search), engine/ui/menus.json (the GroomLab menu), engine/assets/presets (the
// graph's presets, for the search).
import { icon } from "@/data/icons";

// ---- Brushes, in the catalog's groups -----------------------------------------------------------

export type Tool = { id: string; label: string; icon: string; tip: string; action?: boolean };
const t = (id: string, label: string, tip: string, iconName = id, action = false): Tool => ({ id, label, icon: icon(`groomLab_${iconName}`), tip, action });

/** brush_catalog.cpp's categories and order (2026-11-24). Invert and Unfreeze are buttons, not tools. */
export const toolGroups: { id: string; label: string; tools: Tool[] }[] = [
  {
    id: "create", label: "Create", tools: [
      t("place", "Place", "Click the scalp to plant a guide; drag to lay down many"),
      t("draw", "Draw", "Press on the scalp and draw a guide's shape"),
      t("clone", "Clone", "Copy the guides under the brush to where you click (Ctrl+click: pick the source)"),
      t("part", "Part", "Draw a part line the strands split along"),
      t("flow", "Flow", "Lay arrows the fur follows"),
    ],
  },
  {
    id: "shape", label: "Shape", tools: [
      t("comb", "Comb", "Drag guides along the stroke, roots pinned"),
      t("slide", "Slide", "Slide guides over the scalp"),
      t("smooth", "Smooth", "Even out kinks"),
      t("lift", "Lift", "Raise guides off the scalp (Ctrl: lay down)"),
      t("twist", "Swing", "Swing guides around their root"),
      t("attract", "Attract", "Pull guide tips together"),
      t("shape", "Tube", "Grow or shrink the tubes (Shift: smooth)"),
    ],
  },
  {
    id: "trim", label: "Trim", tools: [
      t("cut", "Cut", "Trim guides where the brush touches them"),
      t("length", "Length", "Grow guides evenly (Ctrl: shorten)"),
      t("lengths", "Keys", "Length keys: points on the scalp that set the guides' lengths"),
      t("erase", "Erase", "Remove guides, parts or flow under the brush"),
    ],
  },
  {
    id: "guides", label: "Guides", tools: [
      t("select", "Select", "Click or drag over guides to select them; the rest freeze"),
      t("freeze", "Freeze", "Paint guides frozen: brushes leave them alone"),
      t("invert", "Invert", "Frozen guides thaw, the others freeze", "invert", true),
      t("unfreeze", "Unfreeze", "Unfreeze every guide", "unfreeze", true),
    ],
  },
];
export const allTools = toolGroups.flatMap((g) => g.tools);
export const paintTool = t("paint", "Paint", "Paint the picked map (Shift: smooth, Ctrl: toward 0)");
export const toolById = (id: string) => (id === "paint" ? paintTool : allTools.find((x) => x.id === id)!);

/** What the modifier keys do with each tool: Maya's Help Line shows it while the tool is on. */
export const toolHints: Record<string, string> = {
  place: "Click: plant a guide · Drag: lay down many · Ctrl: erase",
  draw: "Drag from the scalp: draw a guide",
  clone: "Ctrl+click: pick the source · Click: copy it here",
  part: "Drag: draw a part · Ctrl: erase a part",
  flow: "Drag: lay arrows · Drag over arrows: comb them · Ctrl: erase",
  comb: "Drag: comb · Shift: smooth · Ctrl+Shift: paint guides into the selection",
  slide: "Drag: slide over the scalp · Shift: smooth",
  smooth: "Drag: even out kinks",
  lift: "Drag: lift · Ctrl: lay down · Shift: smooth",
  twist: "Drag: swing · Ctrl: the other way · Shift: smooth",
  attract: "Drag: pull tips together · Ctrl: push apart",
  shape: "Drag: grow the tube · Ctrl: shrink · Shift: smooth",
  cut: "Drag: trim where you touch · Shift: smooth",
  length: "Drag: lengthen · Ctrl: shorten",
  lengths: "Click: add a key · Drag a key: set its length",
  erase: "Drag: remove what the brush touches",
  select: "Click / drag: select guides · Shift: add · Ctrl: remove",
  freeze: "Drag: freeze · Ctrl: unfreeze",
  paint: "Drag: paint · Ctrl: toward 0 · Shift: smooth",
};
/** The Tube brush's keys per job (and per root mode). */
export const tubeHints: Record<string, string> = {
  sculpt: "Drag: paint the channel · Ctrl: the opposite · Shift: smooth",
  draw: "Click: add a CV · Click the first CV: close the root · Ctrl+Z: undo a CV",
  edit: "Drag a CV: move it · the tubes follow",
  select: "Click a root's middle: select it · Drag: move it over the scalp",
  shape: "Drag a point in the outline: edit the library shape",
};
export const brushKeys = "B+drag: radius · M+drag: strength · Alt+Shift+X: mirror";

/** What each tool's card shows. `own` = the tool's block under Brush (its essentials only);
 *  the rest of today's card (falloff, curves, Applies to, tube display) goes under "More". */
export type Card = {
  brush: boolean; signed?: boolean; stretch?: boolean; straighten?: boolean; collide?: boolean;
  appliesTo?: boolean; mirror: boolean;
  own?: "newGuides" | "clone" | "parts" | "flow" | "tube" | "keys" | "erase" | "select" | "paint";
};
const shaping: Card = { brush: true, collide: true, mirror: true, appliesTo: true };
export const cards: Record<string, Card> = {
  place: { brush: false, collide: true, mirror: true, own: "newGuides" },
  draw: { brush: false, collide: true, mirror: true, own: "newGuides" },
  clone: { brush: true, mirror: true, own: "clone" },
  part: { brush: false, mirror: true, own: "parts" },
  flow: { brush: true, mirror: true, own: "flow" },
  comb: { ...shaping, stretch: true },
  slide: { ...shaping, appliesTo: false },
  smooth: { ...shaping, straighten: true },
  lift: { ...shaping, signed: true, appliesTo: false },
  twist: { ...shaping, signed: true },
  attract: { ...shaping, signed: true },
  shape: { brush: true, signed: true, mirror: true, own: "tube" },
  cut: { ...shaping },
  length: { ...shaping, signed: true },
  lengths: { brush: false, mirror: true, own: "keys" },
  erase: { brush: true, mirror: true, own: "erase" },
  select: { brush: false, mirror: true, own: "select" },
  freeze: { brush: true, mirror: true },
  paint: { brush: true, mirror: true, own: "paint" },
};

// ---- The groom's lists ---------------------------------------------------------------------------

export const guideGroups = [
  { name: "primary", color: "#7C9CD4", guides: 64 },
  { name: "bangs", color: "#D4B26A", guides: 22 },
  { name: "nape", color: "#6DB897", guides: 18 },
];
export const maps: { name: string; kind: string; color: boolean; readBy: string | null; node?: string }[] = [
  { name: "density", kind: "Scalp vertices", color: false, readBy: "Scatter · Density", node: "scatter" },
  { name: "clump_mask", kind: "Guides", color: false, readBy: "Clump · Strength", node: "clump" },
  { name: "regions", kind: "Texture 2048", color: true, readBy: "Interpolate · Region map", node: "interp" },
  { name: "frizz_tips", kind: "Guides", color: false, readBy: null },
];
/** The graph's mappable params a map can drive ("Use for ▾"): node · param, from the current graph. */
export const mappable = [
  { node: "Scatter", params: ["Density", "Length"] },
  { node: "Clump", params: ["Strength", "Clump Size"] },
  { node: "Noise", params: ["Amplitude", "Fraction"] },
  { node: "Width", params: ["Width"] },
  { node: "Hair Color", params: ["Melanin", "Dye"] },
];
/** brush_catalog.cpp kCorrective: the brushes that edit a corrective layer. */
export const correctiveTools = ["comb", "slide", "smooth", "lift", "twist", "attract", "cut", "length", "select"];
/** choices.json paintMode: how Paint and Fill combine the value with the map. */
export const paintModes = [
  { id: "replace", label: "Replace", tip: "Move values toward the chosen value" },
  { id: "add", label: "Add", tip: "Add the value, clamped to 1" },
  { id: "subtract", label: "Subtract", tip: "Subtract the value, clamped to 0" },
  { id: "clampMaximum", label: "Clamp Max", tip: "Lower values above the chosen maximum, leave the rest" },
];
/** lists.json groups.edits: the Edit Guides menu on the picked group. */
export const editGuides: MenuItem[] = [
  { label: "Generate Children", id: "guides.children" },
  { label: "", sep: true },
  { label: "Mirror", items: [
    { label: "Selected to Other Side", id: "guides.mirrorToOther" }, { label: "Group…", id: "guides.mirrorGroup" }, { label: "", sep: true },
    { label: "Whole Groom +X → -X", id: "guides.syncPos" }, { label: "Whole Groom -X → +X", id: "guides.syncNeg" }, { label: "", sep: true },
    { label: "Select Mirror Guides", id: "guides.mirrorSelection" },
  ] },
  { label: "", sep: true },
  { label: "Reparent…", id: "guides.reparent" },
  { label: "Move to Group", items: [{ label: "bangs" }, { label: "nape" }, { label: "", sep: true }, { label: "New Group", id: "guides.newGroupFromSelection" }] },
  { label: "Copy Guides", id: "guides.copy" },
  { label: "Paste Guides", id: "guides.paste" },
  { label: "From Curves", id: "guides.fromCurves" },
  { label: "Clear Selection", id: "guides.clearSelection" },
  { label: "", sep: true },
  { label: "Reset Segments", id: "group.resetSegments" },
  { label: "Color…", id: "group.color" },
];
export const correctives = [
  { name: "wind_pose", edits: 214, weight: 1 },
  { name: "shot_042_fix", edits: 38, weight: 0.6 },
];
export const photos = [
  { name: "front.jpg", view: "Front" },
  { name: "side_left.jpg", view: "Left" },
];

// ---- The GroomLab menu, regrouped ----------------------------------------------------------------
// Today: ~20 top-level entries (menus.json "main"). Here: what you open, what you start from, files,
// then one submenu per job. Every action id is a real one from engine/ui/actions.json.

export type MenuItem = { label: string; id?: string; kbd?: string; items?: MenuItem[]; sep?: true; danger?: true };
const sep: MenuItem = { label: "", sep: true };
export const groomLabMenu: MenuItem[] = [
  { label: "Groom Tools", id: "tools.open" },
  { label: "Graph Editor", id: "graph.open" },
  { label: "Find Command…", id: "search" },
  sep,
  { label: "Create Groom from Selection", id: "groom.create" },
  { label: "Start From", items: [
    { label: "Nika (head)", id: "sample.nika" }, { label: "Manny (head)", id: "sample.manny" },
    { label: "Wolf", id: "sample.wolf" }, { label: "Buddy (rig)", id: "sample.buddy" }, sep,
    { label: "XGen Description", id: "import.xgen" }, { label: "Yeti Node", id: "import.yeti" }, { label: "Alembic Curves…", id: "import.alembicCurves" },
  ] },
  sep,
  { label: "Groom File", items: [
    { label: "Save", id: "file.save" }, { label: "Save As…", id: "file.saveAs" }, { label: "Import…", id: "file.import" }, { label: "Reload", id: "file.reload" },
  ] },
  { label: "Export", items: [
    { label: "Alembic…", id: "export.alembic" }, { label: "USD…", id: "export.usd" }, { label: "V-Ray Scene…", id: "export.vrscene" }, { label: "XGen Interactive Groom", id: "export.xgenInteractive" }, sep,
    { label: "Unreal Groom…", id: "export.unrealGroom" }, { label: "Unreal Cards…", id: "export.unrealCards" },
    { label: "Unity…", id: "export.unityStrands" }, { label: "glTF Cards…", id: "export.gltfCards" }, { label: "Card Maps…", id: "export.cardMaps" },
  ] },
  { label: "Render", items: [
    { label: "Arnold: From Groom File", id: "render.fromGroomFile" }, { label: "Arnold: As Curves", id: "render.asCurves" }, { label: "Turntable", id: "render.turntable" }, sep,
    { label: "RenderMan…", id: "render.prman" }, { label: "V-Ray Standalone…", id: "render.vrayStandalone" }, sep,
    { label: "Studio Lighting", items: [{ label: "Portrait" }, { label: "Beauty" }, { label: "Rim Drama" }, { label: "High Key" }, sep, { label: "Remove Studio", id: "studio.remove" }] },
  ] },
  { label: "Simulate", items: [
    { label: "Add Simulate Node", id: "sim.add" }, { label: "Simulate Range", id: "sim.range" }, { label: "Bake", id: "sim.bake" }, { label: "Clear", id: "sim.clear" }, sep,
    { label: "Drivers", items: [{ label: "Create Driver Curves", id: "drivers.create" }, { label: "Attach to Rig", id: "drivers.attach" }, { label: "Remove", id: "drivers.remove" }] },
    { label: "Legacy nHair", items: [{ label: "Simulate with nHair", id: "nhair.simulate" }, { label: "Bake", id: "nhair.bake" }, { label: "Remove", id: "nhair.remove" }] },
  ] },
  { label: "Utilities", items: [
    { label: "Card Atlas…", id: "cardAtlas.open" }, { label: "Transfer to Another Scalp…", id: "groom.transfer" }, { label: "Copy Graph to Grooms", id: "graph.copyToGrooms" }, sep,
    { label: "Guides to Curves", id: "guides.toCurves" }, { label: "Curves to Guides", id: "guides.curvesToGuides" }, { label: "Strands to Curves", id: "strands.toCurves" }, { label: "Strands to Mesh", id: "strands.toMesh" }, sep,
    { label: "Find Missing Files…", id: "files.findMissing" }, { label: "Collect Files…", id: "files.collect" },
  ] },
  sep,
  { label: "Show / Hide", items: [{ label: "Show All Hair", id: "display.showAllHair" }, { label: "Hide All Hair", id: "display.hideAllHair" }, sep, { label: "Show All Guides", id: "display.showAllGuides" }, { label: "Hide All Guides", id: "display.hideAllGuides" }] },
  { label: "Settings", items: [{ label: "Theme", items: [{ label: "Charcoal" }, { label: "Studio" }, { label: "Playful" }, { label: "Paper" }] }, { label: "Install Hotkeys", id: "hotkeys.install" }, { label: "Radial Menu…", id: "radial.settings" }] },
  { label: "Help", items: [{ label: "Documentation", id: "help.docs" }, { label: "Shortcuts", id: "help.shortcuts" }, { label: "Check for Updates", id: "help.updates" }, sep, { label: "Discord", id: "help.discord" }, { label: "Donate", id: "help.donate" }, { label: "About", id: "help.about" }] },
];

// ---- Search: every command the hosts have --------------------------------------------------------

type ActionDef = { label: string; tip?: string; hosts?: string[] };
const actionsJson = Object.values(import.meta.glob("../../../../engine/ui/actions.json", { eager: true, import: "default" }))[0];
const actionDefs = (actionsJson as { actions: Record<string, ActionDef> }).actions;
type PresetFile = { about?: { title?: string; description?: string } };
const presetFiles = import.meta.glob("../../../../engine/assets/presets/*.json", { eager: true, import: "default" }) as Record<string, PresetFile>;

export type Command = { id: string; label: string; tip: string; kind: string };
const kindOf = (id: string) => {
  const head = id.split(".")[0];
  const map: Record<string, string> = {
    file: "File", import: "Start", sample: "Start", groom: "Start", photos: "Start", export: "Export", render: "Render", studio: "Render", cards: "Export",
    sim: "Simulate", drivers: "Simulate", nhair: "Simulate", node: "Graph", graph: "Graph", port: "Graph", compound: "Graph", link: "Graph", portal: "Graph",
    backdrop: "Graph", script: "Graph", preset: "Graph", add: "Graph", map: "Paint", mask: "Paint", corrective: "Correctives", sculpt: "Correctives",
    guides: "Guides", group: "Guides", parts: "Guides", flow: "Guides", tube: "Guides", tubes: "Guides", display: "Display", review: "Display", help: "Help",
  };
  return map[head] ?? "Tools";
};
/** Gone in Simple: Guides from Photos (and the camera it adds). */
const dropped = new Set(["photos.guides", "photo.addView"]);
export const commands: Command[] = [
  ...Object.entries(actionDefs)
    .filter(([id, a]) => (!a.hosts || a.hosts.includes("maya")) && !dropped.has(id))
    .map(([id, a]) => ({ id, label: a.label.replace(/\.\.\.$/, "").replace(/&/g, ""), tip: a.tip ?? "", kind: kindOf(id) })),
  ...Object.entries(presetFiles).map(([path, p]) => ({
    id: `preset:${path}`, label: `Preset: ${p.about?.title ?? path}`, tip: p.about?.description ?? "", kind: "Graph",
  })),
];
export const commandCount = Object.keys(actionDefs).length;
export const presetCount = Object.keys(presetFiles).length;
