// SPDX-License-Identifier: Apache-2.0
// Groom Tools, as Maya docks it (a workspaceControl, area="left", ~320 px wide). One column, one
// scroll, top to bottom: the groom → the pages → the brushes (unnamed trays) → what the
// brushes edit (group, layer) → the brush's settings → the page's lists, one line per item with
// icons and popups (no row expands). Built only from parts palette.py and tool_tabs.py already have (QComboBox, tool tiles,
// SoftSlider fields, chips, switch rows, widgets.Section folds, tree lists, the list menus of
// engine/ui/lists.json).
import {
  BringToFront, Brush, Camera, Contrast, GitCommitVertical, Info, ChevronDown, ChevronRight, Eye, EyeOff, Focus, Key as KeyIcon, Layers, Lock, LockOpen, MoreHorizontal,
  Minus, Network, Plus, Search, SendToBack, SlidersHorizontal, Spline, Trash2,
} from "lucide-react";
import { DropdownMenu, Popover } from "radix-ui";
import { useState, type ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { FillField } from "@/components/ui/field";
import { Select } from "@/components/ui/select";
import { Tip } from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";
import {
  cards, correctives, correctiveTools, editGuides, guideGroups, mappable, maps, paintModes, photos, toolById, toolGroups, type MenuItem, type Tool,
} from "./data";
import { Chip, More, Seg } from "./parts";
import { BASE, useS, type Page } from "./state";

// ---- small shared bits ---------------------------------------------------------------------------

const menuRow = "flex h-row cursor-default items-center gap-2 rounded-[4px] px-2 outline-none data-[highlighted]:bg-hover data-[state=open]:bg-hover";
const menuBox = "gl-ui z-50 min-w-48 rounded-ctl border border-line bg-raised p-1 text-ui text-fg";

function MenuItems({ items, onPick }: { items: MenuItem[]; onPick: (i: MenuItem) => void }) {
  return (
    <>
      {items.map((it, i) =>
        it.sep ? <DropdownMenu.Separator key={i} className="my-1 h-px bg-line" /> : it.items ? (
          <DropdownMenu.Sub key={i}>
            <DropdownMenu.SubTrigger className={menuRow}>{it.label}<ChevronRight className="ml-auto size-3 text-muted" /></DropdownMenu.SubTrigger>
            <DropdownMenu.Portal><DropdownMenu.SubContent className={menuBox}><MenuItems items={it.items} onPick={onPick} /></DropdownMenu.SubContent></DropdownMenu.Portal>
          </DropdownMenu.Sub>
        ) : (
          <DropdownMenu.Item key={i} onSelect={() => onPick(it)} className={cn(menuRow, it.danger && "text-error data-[highlighted]:bg-error/15")}>
            {it.danger && <Trash2 className="size-3.5" />}{it.label}
          </DropdownMenu.Item>
        ),
      )}
    </>
  );
}

function MenuButton({ label, items, className, onPick }: { label: ReactNode; items: MenuItem[]; className?: string; onPick?: (i: MenuItem) => void }) {
  const s = useS();
  return (
    <DropdownMenu.Root modal={false}>
      <DropdownMenu.Trigger asChild>
        <Button size="sm" className={cn("gap-1", className)}>{label}<ChevronDown className="size-3 text-muted" /></Button>
      </DropdownMenu.Trigger>
      <DropdownMenu.Portal>
        <DropdownMenu.Content align="start" className={menuBox}>
          <MenuItems items={items} onPick={onPick ?? ((it) => s.say(it.label.replace("…", "")))} />
        </DropdownMenu.Content>
      </DropdownMenu.Portal>
    </DropdownMenu.Root>
  );
}

/** A glyph that opens a menu (the ⋯ on each list row). Qt: an auto-raise QToolButton with InstantPopup. */
function IconMenu({ tip, icon, items, title, onPick }: { tip: string; icon: ReactNode; items: MenuItem[]; title?: string; onPick?: (i: MenuItem) => void }) {
  const s = useS();
  return (
    <DropdownMenu.Root modal={false}>
      <Tip label={tip}>
        <DropdownMenu.Trigger asChild>
          <button className="flex size-6 shrink-0 items-center justify-center rounded-ctl text-muted outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover">{icon}</button>
        </DropdownMenu.Trigger>
      </Tip>
      <DropdownMenu.Portal>
        <DropdownMenu.Content align="end" className={menuBox}>
          {title && <div className="px-2 py-1 text-sm-ui text-muted">{title}</div>}
          <MenuItems items={items} onPick={onPick ?? ((it) => s.say(it.label.replace("…", "")))} />
        </DropdownMenu.Content>
      </DropdownMenu.Portal>
    </DropdownMenu.Root>
  );
}

function Glyph({ tip, children, onClick, on }: { tip: string; children: ReactNode; onClick?: (e: React.MouseEvent) => void; on?: boolean }) {
  return (
    <Tip label={tip}>
      <button onClick={onClick} className={cn("flex size-6 shrink-0 items-center justify-center rounded-ctl outline-none", on ? "text-accent" : "text-muted hover:bg-hover hover:text-fg")}>{children}</button>
    </Tip>
  );
}

function Row({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[84px_1fr] items-center gap-2">
      <span className="truncate text-sm-ui text-dim">{label}</span>
      <div className="flex min-w-0 items-center gap-1">{children}</div>
    </div>
  );
}

function ListHead({ title, count, children }: { title: string; count: number; children?: ReactNode }) {
  return (
    <div className="flex h-row items-center gap-1.5">
      <span className="font-semibold">{title}</span>
      <span className="num text-sm-ui text-muted">{count}</span>
      <div className="ml-auto flex items-center gap-0.5">{children}</div>
    </div>
  );
}

/** Each page section on a card, its header inside (Maya's QWidget[role="tray"], as palette.py puts
 *  every page's list today): the same card as the brush trays and the brush's settings. */
const SECTION = "flex flex-col rounded-pnl border border-line bg-raised/40 p-2";

/** ROW_ORDER: every list row, left to right: the name, then muted text (a count, a tag), then the
 *  icons in this order, each only where the list has it: the settings popup (segments, weight,
 *  ⓘ info, photo sliders) → state toggles (solo, in front / behind) → the eye → the lock → ⋯ the
 *  menu, always last (Rename… first, Delete last in red). The same icon sits in the same place on
 *  every list, and the menu is always at the right edge. */

// ---- header: the groom, then the pages -------------------------------------------------------------

function Header() {
  const s = useS();
  const [hair, setHair] = useState(true);
  const [guides, setGuides] = useState(true);
  return (
    <div className="flex items-center gap-0.5">
      <button className="flex h-row min-w-0 flex-1 items-center gap-1.5 rounded-ctl border border-line bg-raised px-2 text-left outline-none hover:bg-hover">
        <span className="size-2 shrink-0 rounded-full bg-accent" />
        <span className="flex-1 truncate font-semibold">{s.hasGroom ? "manny_hair" : "No groom"}</span>
        <ChevronDown className="size-3.5 shrink-0 text-muted" />
      </button>
      <Tip label="Show the hair" shortcut="Shift+click: every groom's">
        <Button variant="ghost" size="iconSm" onClick={() => setHair(!hair)} disabled={!s.hasGroom}>{hair ? <Eye className="size-4" /> : <EyeOff className="size-4" />}</Button>
      </Tip>
      {/* A split button: the icon shows or hides the guides, the arrow opens how they draw.
          Qt: a QToolButton with MenuButtonPopup (checkable; the arrow part opens its menu). */}
      <DropdownMenu.Root modal={false}>
        <div className={cn("flex h-[calc(var(--gl-row)-4px)] items-center rounded-ctl", !s.hasGroom && "pointer-events-none opacity-40")}>
          <Tip label={guides ? "Hide the guides" : "Show the guides"} shortcut="Shift+click: every groom's">
            <button onClick={() => setGuides(!guides)} className={cn("flex h-full w-6 items-center justify-center rounded-l-ctl outline-none hover:bg-hover", guides ? "text-dim hover:text-fg" : "text-muted opacity-50")}>
              <Spline className="size-4" />
            </button>
          </Tip>
          <Tip label="How the guides draw: width, points, colors, flow, tubes">
            <DropdownMenu.Trigger asChild>
              <button className="flex h-full w-3.5 items-center justify-center rounded-r-ctl border-l border-line/70 text-muted outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover">
                <ChevronDown className="size-2.5" />
              </button>
            </DropdownMenu.Trigger>
          </Tip>
        </div>
        <DropdownMenu.Portal>
          <DropdownMenu.Content align="end" className="gl-ui z-50 w-60 rounded-ctl border border-line bg-raised p-2 text-ui text-fg">
            <div className="flex flex-col gap-1.5">
              <div className="flex flex-wrap gap-1">
                <Chip on={guides} onClick={() => setGuides(!guides)}>Guides</Chip><Chip on onClick={() => {}}>Flow</Chip><Chip on={false} onClick={() => {}}>Tubes</Chip><Chip on onClick={() => {}}>Instances</Chip>
              </div>
              <FillField label="Width" value={2} min={1} max={6} step={0.5} onChange={() => {}} />
              <Row label="Points"><Select value="Roots and tips" options={["Off", "Roots and tips", "All"]} className="flex-1" /></Row>
              <Row label="Colors"><Select value="Group" options={["Group", "Painted Layer"]} className="flex-1" /></Row>
              <div className="flex flex-wrap gap-1"><Chip on onClick={() => {}}>On top</Chip><Chip on={false} onClick={() => {}}>Smoothed</Chip></div>
              <div className="mt-1 text-sm-ui font-semibold text-dim">Tubes</div>
              <div className="flex flex-wrap gap-1">
                <Chip on={false} onClick={() => {}} tip="Draw the tubes as shaded surfaces">Shaded</Chip><Chip on onClick={() => {}} tip="See-through wires">Wireframe</Chip>
                <Chip on onClick={() => {}} tip="Each tube in its root tube's color">Colors</Chip><Chip on={false} onClick={() => {}} tip="Each split level a shade of its own">Level colors</Chip>
                <Chip on={false} onClick={() => {}} tip="Keep the tubes drawn with every tool, not only Tube">With every tool</Chip>
              </div>
            </div>
          </DropdownMenu.Content>
        </DropdownMenu.Portal>
      </DropdownMenu.Root>
      <Tip label="Graph Editor: the groom's look">
        <Button variant={s.graph ? "checked" : "ghost"} size="iconSm" onClick={() => s.setGraph(!s.graph)} disabled={!s.hasGroom}><Network className="size-4" /></Button>
      </Tip>
      <Tip label="Find any GroomLab command">
        <Button variant="ghost" size="iconSm" onClick={() => s.setSearch(true)}><Search className="size-4" /></Button>
      </Tip>
      <Tip label="New groom, files, settings">
        <Button variant="ghost" size="iconSm"><MoreHorizontal className="size-4" /></Button>
      </Tip>
    </div>
  );
}

const pages: { id: Page; label: string }[] = [
  { id: "tools", label: "Tools" },
  { id: "paint", label: "Paint" },
  { id: "photos", label: "Photos" },
];

// ---- the brushes: unnamed trays, transparent one-tone tiles --------------------------------------

/** One-tone icon: the SVG's shape in the text color (the chosen tool's in the accent). Qt: the
 *  app's tinted_icon(). */
function BrushIcon({ tool, size = 20, className }: { tool: Tool; size?: number; className?: string }) {
  return <span className={cn("icon-mono inline-block shrink-0", className)} style={{ width: size, height: size, ["--icon" as string]: `url("${tool.icon}")` }} />;
}

function Tile({ tool }: { tool: Tool }) {
  const s = useS();
  const on = s.tool === tool.id;
  const off = s.layer !== BASE && !tool.action && !correctiveTools.includes(tool.id);
  return (
    <Tip label={<span className="block max-w-56">{off ? `${tool.label} doesn't edit a corrective layer: pick Base groom to use it` : tool.tip}</span>}>
      <button
        onClick={() => (off ? null : tool.action ? s.say(tool.id === "invert" ? "Inverted the frozen guides" : "Unfroze every guide") : s.setTool(tool.id))}
        className={cn(
          "flex h-[44px] flex-col items-center justify-center gap-[3px] rounded-ctl border outline-none",
          // Maya's QToolButton[role="tool"]: transparent, hover tint, the chosen one on the soft accent with its edge.
          on ? "border-accent/60 bg-selection text-fg" : off ? "cursor-default border-transparent text-muted opacity-35" : "border-transparent text-dim hover:bg-hover hover:text-fg",
        )}
      >
        <BrushIcon tool={tool} className={on ? "text-accent" : ""} />
        <span className="w-full truncate text-center text-[10px] leading-3 tracking-[-0.02em]">{tool.label}</span>
      </button>
    </Tip>
  );
}

function Brushes() {
  return (
    <div className="flex flex-col gap-1.5">
      {toolGroups.map((g) => (
        // Qt: the existing trays (QWidget[role="tray"]), each group's tiles in one; only the label is gone.
        <div key={g.id} title={g.label} className="grid grid-cols-5 gap-0.5 rounded-pnl bg-raised/50 p-1">
          {g.tools.map((t) => <Tile key={t.id} tool={t} />)}
        </div>
      ))}
    </div>
  );
}

// ---- what the brushes edit: the picked group, and the base groom or a corrective layer -----------

/** The layer brushes edit, shown in the card's header: plain text (the Correctives section below the
 *  guide groups picks it). On a corrective it is in the accent, so you can't miss sculpting on one. */
function LayerTag() {
  const s = useS();
  const base = s.layer === BASE;
  return (
    <Tip label={base ? "Brushes edit the base groom (pick a corrective in Correctives below)" : "Brushes edit this corrective (pick Base groom in Correctives below to leave it)"}>
      <span className={cn("flex min-w-0 items-center gap-1 rounded-ctl px-1", base ? "text-fg" : "bg-selection text-fg ring-1 ring-accent/60")}>
        <Layers className={cn("size-3 shrink-0", base ? "text-muted" : "text-accent")} /><span className="truncate">{s.layer}</span>
      </span>
    </Tip>
  );
}

/** Correctives: sculpt layers on top of the groom, under the guide groups, one line per layer like
 *  the groups and maps. Clicking a layer makes the brushes edit it (brushes that can't are greyed);
 *  clicking it again, or Base groom, goes back. Each row: the name, its edit count, then icons: ◐ 1.0
 *  the weight (a popup: Weight and its key), Solo, ⋯ (Move Up/Down, Go to Its Frame, Key Weight Here,
 *  Clear Keys, Rename, Delete in red). Qt: tool_tabs.CorrectivesBox
 *  as a widgets.Section on the Tools page, the glyphs drawn by the row's item delegate. */
function Correctives() {
  const s = useS();
  const [weights, setWeights] = useState<Record<string, number>>(Object.fromEntries(correctives.map((c) => [c.name, c.weight])));
  const [solo, setSolo] = useState<string | null>(null);
  const more: MenuItem[] = [
    { label: "Rename…" }, { label: "", sep: true },
    { label: "Move Up" }, { label: "Move Down" }, { label: "", sep: true },
    { label: "Go to Its Frame" }, { label: "Key Weight Here" }, { label: "Clear Keys (every frame)" }, { label: "", sep: true },
    { label: "Delete Corrective", danger: true },
  ];
  return (
    <div className={SECTION}>
      <ListHead title="Correctives" count={correctives.length}>
        <Tip label="A corrective on this frame, keyed to blend in and out around it"><Button size="sm" variant="ghost" className="h-6 px-1.5 text-sm-ui" onClick={() => s.say("Fix Frame: a corrective keyed at frame 8")}>◆ Fix Frame</Button></Tip>
        <Glyph tip="New corrective"><Plus className="size-3.5" /></Glyph>
      </ListHead>
      <div className="flex flex-col gap-0.5">
        {correctives.map((x) => {
          const picked = s.layer === x.name;
          const w = weights[x.name];
          return (
            <div
              key={x.name}
              onClick={() => s.setLayer(picked ? BASE : x.name)}
              className={cn("flex h-row cursor-default items-center gap-1.5 rounded-ctl pl-1.5", picked ? "bg-selection text-fg" : "text-dim hover:bg-hover", solo && solo !== x.name && "opacity-55")}
            >
              <Layers className={cn("size-3 shrink-0", picked ? "text-accent" : "text-muted")} />
              <span className="min-w-0 flex-1 truncate">{x.name}</span>
              <Tip label={`${x.edits} sculpt edits`}><span className="num shrink-0 text-sm-ui text-muted">{x.edits}</span></Tip>
              <div className="flex items-center" onClick={(e) => e.stopPropagation()}>
                <Popover.Root>
                  <Tip label={`Weight ${w.toFixed(2)}`}>
                    <Popover.Trigger asChild>
                      <button className="flex h-6 shrink-0 items-center gap-0.5 rounded-ctl px-1 text-muted outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover">
                        <Contrast className="size-3.5" /><span className="num text-[10.5px]">{w.toFixed(1)}</span>
                      </button>
                    </Popover.Trigger>
                  </Tip>
                  <Popover.Portal>
                    <Popover.Content align="end" sideOffset={4} className="gl-ui z-50 flex w-60 items-center gap-1 rounded-pnl border border-line bg-panel p-2 text-ui text-fg">
                      <FillField label="Weight" value={w} onChange={(v) => setWeights({ ...weights, [x.name]: v })} className="flex-1" />
                      <Glyph tip="Key the weight on this frame (S)"><KeyIcon className="size-3.5" /></Glyph>
                    </Popover.Content>
                  </Popover.Portal>
                </Popover.Root>
                <Glyph tip={solo === x.name ? "Solo is on: click for all on" : "Solo: only this corrective on"} on={solo === x.name} onClick={() => setSolo(solo === x.name ? null : x.name)}><Focus className="size-3.5" /></Glyph>
                <IconMenu tip="Move, go to its frame, keys, rename, delete" icon={<MoreHorizontal className="size-3.5" />} items={more} onPick={(it) => s.say(`${it.label}: ${x.name}`)} />
              </div>
            </div>
          );
        })}
        <div
          onClick={() => s.setLayer(BASE)}
          className={cn("flex h-row cursor-default items-center gap-1.5 rounded-ctl px-1.5", s.layer === BASE ? "bg-selection text-fg" : "text-dim hover:bg-hover")}
        >
          <Layers className="size-3 shrink-0 text-muted" />
          <span className="flex-1 truncate">{BASE}</span>
          <span className="text-sm-ui text-muted">base</span>
        </div>
      </div>
      {s.layer !== BASE && <span className="mt-1 text-[10.5px] text-muted">Brushes edit {s.layer}; the ones that can't are greyed out.</span>}
      <div className="mt-1">
        <More label="Display while sculpting">
          <Row label="Strands"><Select value="Strands as they are" options={["Strands as they are", "Strands dimmed", "Guides only"]} className="flex-1" /></Row>
          <FillField label="Strand density" value={0.25} onChange={() => {}} />
        </More>
      </div>
    </div>
  );
}

/** The card's header: the brush's icon and name, then what the next stroke changes (the picked
 *  group, and the base groom or a corrective layer). */
function CardHead() {
  const s = useS();
  const tool = toolById(s.tool);
  const g = guideGroups.find((x) => x.name === s.group)!;
  return (
    <div className="flex min-w-0 items-center gap-1.5">
      <BrushIcon tool={tool} size={18} className="text-accent" />
      <span className="shrink-0 font-semibold">{tool.label}</span>
      <span className="ml-auto flex min-w-0 items-center gap-1 text-sm-ui text-muted">
        <Tip label="The picked guide group: brushes edit it, new guides go into it"><span className="flex min-w-0 items-center gap-1 text-fg"><span className="size-2 shrink-0 rounded-full" style={{ background: g.color }} /><span className="truncate">{g.name}</span></span></Tip>
        <span className="shrink-0">on</span>
        <LayerTag />
      </span>
    </div>
  );
}

// ---- the brush's settings -----------------------------------------------------------------------

function OwnBlock({ own }: { own: NonNullable<(typeof cards)[string]["own"]> }) {
  const [v, setV] = useState<Record<string, number>>({ length: 4, segments: 8, soft: 0 });
  const set = (k: string) => (x: number) => setV({ ...v, [k]: x });
  switch (own) {
    case "newGuides":
      return (
        <>
          <FillField label="Length" value={v.length} max={20} step={0.1} suffix=" cm" onChange={set("length")} />
          <FillField label="Segments" value={v.segments} min={2} max={32} step={1} onChange={set("segments")} />
        </>
      );
    case "clone":
      return <div className="flex gap-1"><Button size="sm" className="flex-1">Pick Source</Button><Button size="sm" variant="ghost">Reset</Button></div>;
    case "parts":
      return <Row label="Next part"><input defaultValue="part" className="h-row min-w-0 flex-1 select-text rounded-ctl border border-line bg-raised px-2 text-fg outline-none focus:border-accent" /></Row>;
    case "flow":
      return <Row label="Click lays"><Select value="Arrow" options={["Arrow", "Whorl", "Sink"]} className="flex-1" /></Row>;
    case "tube":
      return <Seg size="sm" items={[{ id: "width", label: "Width" }, { id: "flat", label: "Flatness" }, { id: "twist", label: "Twist" }]} value="width" onChange={() => {}} />;
    case "keys":
      return <span className="text-sm-ui text-muted">3 keys · click the scalp to add one. <button className="text-accent">Apply</button></span>;
    case "erase":
      return <Row label="Removes"><Chip on onClick={() => {}}>Guides</Chip><Chip on onClick={() => {}}>Parts</Chip><Chip on onClick={() => {}}>Flow</Chip></Row>;
    case "select":
      return <FillField label="Soft radius" value={v.soft} step={0.01} suffix=" cm" onChange={set("soft")} />;
    case "paint":
      return null;
  }
}

// ---- the Tube brush: three jobs in one card ------------------------------------------------------
// Today the Tube card stacks every block at once (all foldable, issue 70): Brush with Paints,
// Visibility, Profile with its editor, Tube roots, Tube hierarchy, Selected guides. Here one switch
// picks the job (Sculpt · Roots · Shape) and the card shows that job only. The level row (the
// tube hierarchy) and the selected tubes stay in every job: they are what you work on. The tube
// display switches go to the guides ▾ menu in the header (Tubes), with the other display settings.

/** A tube cross-section, drawn from its built-in shape (choices.json tubeShape) with the flatness
 *  and twist applied. Qt: groomlab.ui.tube_shape_editor (the editor) paints the same outline. */
function outline(shape: string, flat: number, n = 24): [number, number][] {
  const pts: [number, number][] = [];
  for (let i = 0; i < n; i++) {
    const a = (i / n) * Math.PI * 2;
    let x = Math.cos(a), y = Math.sin(a);
    if (shape === "ribbon") { const k = 4; x = Math.sign(x) * Math.pow(Math.abs(x), 2 / k); y = Math.sign(y) * Math.pow(Math.abs(y), 2 / k); }
    if (shape === "teardrop") { y = y > 0 ? y : y * 1.6; x = y < 0 ? x * (1 + y / 1.6 * 0.85) : x; }
    if (shape === "blade") { x = x * 1.25; y = y * Math.pow(Math.abs(Math.cos(a)), 0.15) * (1 - Math.abs(Math.cos(a)) * 0.0) * 0.7; }
    pts.push([x, y * (1 - flat * 0.85)]);
  }
  return pts;
}

function ShapePreview({ shape, flat, size = 22, editor = false }: { shape: string; flat: number; size?: number; editor?: boolean }) {
  const pts = outline(shape, flat, editor ? 16 : 32);
  const c = size / 2, r = size * (editor ? 0.36 : 0.42);
  const d = pts.map(([x, y], i) => `${i ? "L" : "M"}${(c + x * r).toFixed(1)} ${(c - y * r).toFixed(1)}`).join(" ") + "Z";
  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} className="block shrink-0">
      {editor && (
        <>
          {[0.25, 0.5, 0.75].map((t) => <g key={t}><line x1={size * t} y1={0} x2={size * t} y2={size} stroke="var(--gl-border)" /><line x1={0} y1={size * t} x2={size} y2={size * t} stroke="var(--gl-border)" /></g>)}
          <line x1={c} y1={4} x2={c} y2={size - 4} stroke="var(--gl-muted)" strokeDasharray="3 3" />
          <text x={c + 4} y={12} fontSize={9} fill="var(--gl-muted)">up</text>
        </>
      )}
      <path d={d} fill={editor ? "color-mix(in srgb, var(--gl-accent) 18%, transparent)" : "none"} stroke={editor ? "var(--gl-accent)" : "currentColor"} strokeWidth={editor ? 1.5 : 1.2} />
      {editor && pts.map(([x, y], i) => <circle key={i} cx={c + x * r} cy={c - y * r} r={3} fill="var(--gl-panel)" stroke="var(--gl-accent)" strokeWidth={1.2} />)}
    </svg>
  );
}

const tubeShapes = [
  { id: "round", label: "Round" }, { id: "ribbon", label: "Ribbon" }, { id: "teardrop", label: "Teardrop" }, { id: "blade", label: "Blade" },
  { id: "lock_s", label: "lock_s (library)" },
];

/** The tube hierarchy as one row: the levels (1 = the base), + splits the selected tubes (else
 *  every tube) a level deeper, − undoes a split. Picking a level makes brushes edit its tubes. */
function Levels() {
  const [levels, setLevels] = useState(2);
  const [level, setLevel] = useState(1);
  return (
    <div className="flex items-center gap-1">
      <span className="w-12 text-sm-ui text-dim">Level</span>
      <div className="flex gap-0.5">
        {Array.from({ length: levels }).map((_, i) => (
          <Tip key={i} label={`Level ${i + 1}${i === 0 ? " (the base tubes)" : ""}: show its tubes and edit them`}>
            <button onClick={() => setLevel(i + 1)} className={cn("num size-6 rounded-ctl border text-sm-ui outline-none", level === i + 1 ? "border-accent/60 bg-selection text-fg" : "border-line text-dim hover:bg-hover")}>{i + 1}</button>
          </Tip>
        ))}
      </div>
      <div className="ml-auto flex">
        <Glyph tip="Split the selected tubes (else every tube) in two, a level deeper" onClick={() => setLevels(Math.min(6, levels + 1))}><Plus className="size-3.5" /></Glyph>
        <Glyph tip="Undo a split (the selected tubes', else the deepest level)" onClick={() => { setLevels(Math.max(1, levels - 1)); setLevel(Math.min(level, Math.max(1, levels - 1))); }}><Minus className="size-3.5" /></Glyph>
        <IconMenu tip="Level view" icon={<MoreHorizontal className="size-3.5" />} items={[{ label: "Lock view" }, { label: "Mixed view" }, { label: "Colors per level" }]} />
      </div>
    </div>
  );
}

function TubeSculpt() {
  const [channel, setChannel] = useState("width");
  const [radius, setRadius] = useState(60);
  const [strength, setStrength] = useState(0.5);
  const [pressure, setPressure] = useState(true);
  const s = useS();
  return (
    <>
      <Row label="Paints"><Seg size="sm" className="flex-1" items={[{ id: "width", label: "Width", tip: "Grow the tubes (Ctrl: shrink)" }, { id: "flatness", label: "Flatness", tip: "Flatten toward a ribbon (Ctrl: round back)" }, { id: "twist", label: "Twist", tip: "Turn the tubes about their guides (Ctrl: the other way)" }]} value={channel} onChange={setChannel} /></Row>
      <FillField label="Radius" value={radius} min={1} max={300} step={1} suffix=" px" onChange={setRadius} />
      <FillField label="Strength" value={strength} min={-1} max={1} onChange={setStrength} />
      <div className="flex flex-wrap gap-1">
        <Chip on={s.mirror} onClick={() => s.setMirror(!s.mirror)} tip="Mirror strokes across the head (Alt+Shift+X)">⇆ Mirror</Chip>
        <Chip on={pressure} onClick={() => setPressure(!pressure)}>Pressure</Chip>
      </div>
      <More label="More settings">
        <FillField label="Falloff" value={0.5} onChange={() => {}} />
        <Row label="Shift smooths"><Select value="Current channel" options={["Current channel", "All channels"]} className="flex-1" /></Row>
        <div className="flex flex-wrap gap-1"><Chip on={false} onClick={() => {}}>Falloff curve</Chip><Chip on={false} onClick={() => {}}>Root to tip</Chip></div>
      </More>
    </>
  );
}

function TubeRoots() {
  const s = useS();
  const [length, setLength] = useState(4);
  const [segments, setSegments] = useState(12);
  const [tip, setTip] = useState(0.5);
  const [snap, setSnap] = useState(true);
  const [extrude, setExtrude] = useState(false);
  const mode = s.rootMode;
  return (
    <>
      <Seg
        size="sm"
        items={[
          { id: "draw", label: "Draw", tip: "Click the root's outline on the scalp, CV by CV; click the first CV to close it" },
          { id: "edit", label: "Edit", tip: "Drag the roots' CVs (the selected guides', else all)" },
          { id: "select", label: "Select", tip: "Click a root's middle to select it; drag to move it over the scalp" },
        ]}
        value={mode}
        onChange={s.setRootMode}
      />
      {mode === "draw" && (
        <>
          <div className="flex items-center gap-1">
            <span className="flex-1 text-[10.5px] text-muted">5 CVs · click the first one to close</span>
            <Button size="sm" variant="ghost" onClick={() => s.say("Undid the last CV")}>Undo CV</Button>
          </div>
          <span className="text-sm-ui text-dim">New tubes</span>
          <FillField label="Length" value={length} max={20} step={0.1} suffix=" cm" onChange={setLength} />
          <FillField label="Segments" value={segments} min={2} max={32} step={1} onChange={setSegments} />
          <FillField label="Tip width" value={tip} onChange={setTip} />
          <div className="flex flex-wrap gap-1">
            <Chip on={snap} onClick={() => setSnap(!snap)} tip="A CV near a neighbouring root's CV or edge snaps onto it">Snap to neighbours</Chip>
            <Chip on={extrude} onClick={() => setExtrude(!extrude)} tip="Once the outline closes, drag from its middle to draw the guide">Draw guide</Chip>
          </div>
          <More label="More settings"><FillField label="Point density" value={1} min={0.25} max={4} onChange={() => {}} /></More>
        </>
      )}
      {mode === "edit" && (
        <div className="flex items-center gap-1">
          <span className="flex-1 text-[10.5px] text-muted">Drag a CV; the tubes follow.</span>
          <Button size="sm" variant="ghost" onClick={() => s.say("Undid the last CV edit")}>Undo CV</Button>
          <Button size="sm" onClick={() => { s.setRootMode("select"); s.say("Finished editing roots"); }}>Finish</Button>
        </div>
      )}
      {mode === "select" && (
        <>
          <div className="flex items-center gap-1">
            <span className="flex-1 truncate text-sm-ui text-fg">root_07 <span className="text-muted">selected</span></span>
            <Tip label="Copy the selected root; the copy follows the cursor until you click"><Button size="sm" variant="ghost" onClick={() => s.say("Duplicated root_07")}>Duplicate</Button></Tip>
            <IconMenu tip="More for the selected root" icon={<MoreHorizontal className="size-3.5" />} onPick={(it) => s.say(`${it.label}: root_07`)} items={[{ label: "Subdivide into cells" }, { label: "", sep: true }, { label: "Delete Root", danger: true }]} />
          </div>
          <More label="Subdivide">
            <div className="flex items-center gap-1">
              <FillField label="Multiplier" value={1} min={0.25} max={4} onChange={() => {}} className="flex-1" />
              <Tip label="Paint where cells come smaller (the subdivision density map)"><Button size="sm" variant="ghost" onClick={() => s.say("Painting the subdivision density")}><Brush className="size-3.5" /></Button></Tip>
              <Button size="sm" onClick={() => s.say("Cut the subdivided roots again")}>Apply</Button>
            </div>
          </More>
        </>
      )}
    </>
  );
}

function TubeShape() {
  const s = useS();
  const [profile, setProfile] = useState<"root" | "mid" | "tip">("root");
  const [shapes, setShapes] = useState<Record<string, string>>({ root: "round", mid: "", tip: "teardrop" });
  const [holds, setHolds] = useState<Record<string, number>>({ root: 20, mid: 0, tip: 30 });
  const flat = 0.3; // the targets' flatness (Selected tubes)
  const shown = shapes[profile] || "round";
  return (
    <>
      <div className="grid grid-cols-3 gap-1">
        {(["root", "mid", "tip"] as const).map((p) => (
          <button
            key={p}
            onClick={() => setProfile(p)}
            className={cn("flex flex-col items-center gap-0.5 rounded-ctl border py-1 outline-none", profile === p ? "border-accent/60 bg-selection text-fg" : "border-line text-dim hover:bg-hover")}
          >
            {shapes[p] ? <ShapePreview shape={shapes[p]} flat={flat} /> : <span className="flex size-[22px] items-center justify-center text-[10px] text-muted">—</span>}
            <span className="text-[10.5px] capitalize">{p}</span>
          </button>
        ))}
      </div>
      <div className="flex items-start gap-2">
        <div className="rounded-ctl border border-line bg-background/60"><ShapePreview shape={shown} flat={flat} size={132} editor /></div>
        <div className="flex min-w-0 flex-1 flex-col gap-1">
          <Select value={tubeShapes.find((t) => t.id === shown)?.label ?? "Round"} options={tubeShapes.map((t) => t.label)} onChange={(label) => setShapes({ ...shapes, [profile]: tubeShapes.find((t) => t.label === label)?.id ?? "round" })} className="w-full" />
          <Button size="sm" onClick={() => s.say(`Gave the selected guides this ${profile} profile`)}>Apply</Button>
          <FillField label="Hold" value={holds[profile]} min={0} max={100} step={1} suffix="%" onChange={(v) => setHolds({ ...holds, [profile]: v })} />
          <div className="flex flex-wrap gap-1"><Chip on onClick={() => {}} tip="Keep the left and right sides alike">Mirror</Chip><Chip on onClick={() => {}} tip="A smooth curve through the points">Smooth</Chip></div>
        </div>
      </div>
      <div className="flex items-center gap-1">
        <span className="flex-1 text-[10.5px] text-muted">{profile === "mid" && !shapes.mid ? "Mid: halfway between root and tip" : "Drag a point to edit (library shapes)"}</span>
        <IconMenu tip="Shape library" icon={<MoreHorizontal className="size-3.5" />} onPick={(it) => s.say(it.label)} items={[{ label: "Rename…" }, { label: "New Shape" }, { label: "Duplicate" }, { label: "", sep: true }, { label: "Delete Shape", danger: true }]} />
      </div>
    </>
  );
}

/** The guides' tubes the edits go to: the selected guides (else the soloed ones). Width, Flatness
 *  and Twist as numbers, Smooth to even them out. In every job. */
function SelectedTubes() {
  const [w, setW] = useState(0.1), [f, setF] = useState(0.3), [t, setT] = useState(0);
  const s = useS();
  return (
    <More label="Selected tubes · 12">
      <FillField label="Width" value={w} step={0.01} suffix=" cm" onChange={setW} />
      <FillField label="Flatness" value={f} max={0.99} onChange={setF} />
      <FillField label="Twist" value={t} min={-180} max={180} step={1} suffix="°" onChange={setT} />
      <Button size="sm" variant="ghost" className="self-start" onClick={() => s.say("Smoothed the selected tubes")}>Smooth</Button>
    </More>
  );
}

function TubeCard() {
  const s = useS();
  return (
    <div className="flex flex-col gap-1.5 rounded-pnl border border-line bg-raised/40 p-2">
      <CardHead />
      <Seg
        items={[
          { id: "sculpt", label: "Sculpt", tip: "Brush the tubes: grow, flatten or twist them where you paint" },
          { id: "roots", label: "Roots", tip: "Draw, edit and select root tubes on the scalp" },
          { id: "shape", label: "Shape", tip: "The tube's cross-section: root, mid and tip profiles" },
        ]}
        value={s.tubeMode}
        onChange={s.setTubeMode}
      />
      <Levels />
      <div className="my-0.5 h-px bg-line" />
      {s.tubeMode === "sculpt" ? <TubeSculpt /> : s.tubeMode === "roots" ? <TubeRoots /> : <TubeShape />}
      <div className="my-0.5 h-px bg-line" />
      <SelectedTubes />
    </div>
  );
}

function ToolCard() {
  const s = useS();
  const card = cards[s.tool];
  const [radius, setRadius] = useState(60);
  const [strength, setStrength] = useState(card?.signed ? 0.5 : 1);
  const [falloff, setFalloff] = useState(0.5);
  const [pressure, setPressure] = useState(true);
  const [collide, setCollide] = useState(true);
  const [stretch, setStretch] = useState(true);
  const [straighten, setStraighten] = useState(false);
  const [match, setMatch] = useState(true);
  if (s.tool === "shape") return <TubeCard />;
  if (!card) return null;
  const chips = card.mirror || card.brush || card.collide || card.stretch || card.straighten || card.own === "newGuides";
  return (
    <div className="flex flex-col gap-1.5 rounded-pnl border border-line bg-raised/40 p-2">
      <CardHead />
      {card.brush && (
        <>
          <FillField label="Radius" value={radius} min={1} max={300} step={1} suffix=" px" onChange={setRadius} />
          <FillField label="Strength" value={strength} min={card.signed ? -1 : 0} max={1} onChange={setStrength} />
        </>
      )}
      {card.own && <OwnBlock own={card.own} />}
      {chips && (
        <div className="flex flex-wrap gap-1">
          {card.mirror && <Chip on={s.mirror} onClick={() => s.setMirror(!s.mirror)} tip="Mirror strokes across the head (Alt+Shift+X)">⇆ Mirror</Chip>}
          {card.brush && <Chip on={pressure} onClick={() => setPressure(!pressure)} tip="Pen pressure scales the strength">Pressure</Chip>}
          {card.collide && <Chip on={collide} onClick={() => setCollide(!collide)} tip="Keep the guides off the scalp">Keep off scalp</Chip>}
          {card.stretch && <Chip on={stretch} onClick={() => setStretch(!stretch)} tip="Guides lengthen or shorten with the pull">Stretch</Chip>}
          {card.straighten && <Chip on={straighten} onClick={() => setStraighten(!straighten)} tip="Smooth toward a straight line">Straighten</Chip>}
          {card.own === "newGuides" && <Chip on={match} onClick={() => setMatch(!match)} tip="New guides take the shape, length and maps of their neighbours">Match nearby</Chip>}
        </div>
      )}
      {(card.brush || card.mirror || card.appliesTo || card.own === "newGuides" || card.own === "tube") && (
        <More label="More settings">
          {card.brush && <FillField label="Falloff" value={falloff} onChange={setFalloff} />}
          {card.mirror && <Row label="Mirror across"><Select value={s.mirrorAxis} options={["World X", "Scalp topology"]} className="flex-1" /></Row>}
          {card.appliesTo && <Row label="Applies to"><Seg size="sm" className="flex-1" items={[{ id: "g", label: "Guides" }, { id: "p", label: "Parts" }, { id: "f", label: "Flow" }]} value="g" onChange={() => {}} /></Row>}
          {card.brush && <div className="flex flex-wrap gap-1"><Chip on={false} onClick={() => {}}>Falloff curve</Chip><Chip on={false} onClick={() => {}}>Root to tip</Chip></div>}
          {card.own === "newGuides" && <><FillField label="Spacing" value={0.5} suffix=" cm" onChange={() => {}} /><div className="flex gap-1"><Chip on={false} onClick={() => {}}>Solo new</Chip></div></>}
        </More>
      )}
    </div>
  );
}

// ---- Tools: guide groups, one line each ---------------------------------------------------------

/** Guide groups: one line per group. The dot, the name, the guide count, then the icons in the
 *  order every list uses (ROW_ORDER): ⫶12 the segments popup (Segments, By length, what they apply
 *  to), the eye, then ⋯ last (Rename…, Select Guides, Generate Children ▸ a count, Mirror ▸,
 *  Reparent, Move to Group ▸ …, Delete Group in red). Qt: item-delegate glyphs on the tree row. */
function GuideGroups() {
  const s = useS();
  const [isolate, setIsolate] = useState(false);
  const [segments, setSegments] = useState<Record<string, number>>({ primary: 12, bangs: 10, nape: 8 });
  const [byLength, setByLength] = useState(false);
  const [shown, setShown] = useState<Record<string, boolean>>(Object.fromEntries(guideGroups.map((g) => [g.name, true])));
  const edits: MenuItem[] = [
    { label: "Rename…" },
    { label: "Select Guides" },
    { label: "", sep: true },
    { label: "Generate Children", items: [1, 2, 3, 4, 6, 8].map((n) => ({ label: `${n} per guide` })) },
    ...editGuides.slice(1),
    { label: "", sep: true },
    { label: "Delete Group", danger: true },
  ];
  return (
    <div className={SECTION}>
      <ListHead title="Guide groups" count={guideGroups.length}>
        <Glyph tip="Isolate: the picked group at full color, the others dimmed" on={isolate} onClick={() => setIsolate(!isolate)}><Focus className="size-3.5" /></Glyph>
        <Glyph tip="New group (from the selected guides)"><Plus className="size-3.5" /></Glyph>
      </ListHead>
      <div className="flex flex-col gap-0.5">
        {guideGroups.map((g) => {
          const picked = s.group === g.name;
          const n = segments[g.name];
          return (
            <div
              key={g.name}
              onClick={() => s.setGroup(g.name)}
              className={cn("flex h-row cursor-default items-center gap-1.5 rounded-ctl pl-1.5", picked ? "bg-selection text-fg" : "text-dim hover:bg-hover", !shown[g.name] && "opacity-55")}
            >
              <Tip label="Group color (click to change)"><span className="size-2.5 shrink-0 rounded-full" style={{ background: g.color }} /></Tip>
              <span className="min-w-0 flex-1 truncate">{g.name}</span>
              <span className="num shrink-0 text-sm-ui text-muted">{g.guides}</span>
              <div className="flex items-center" onClick={(e) => e.stopPropagation()}>
                <Popover.Root>
                  <Tip label={`${n} segments${byLength ? " (by length)" : ""}`}>
                    <Popover.Trigger asChild>
                      <button className="flex h-6 shrink-0 items-center gap-0.5 rounded-ctl px-1 text-muted outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover">
                        <GitCommitVertical className="size-3.5" /><span className="num text-[10.5px]">{n}</span>
                      </button>
                    </Popover.Trigger>
                  </Tip>
                  <Popover.Portal>
                    <Popover.Content align="end" sideOffset={4} className="gl-ui z-50 flex w-60 flex-col gap-1.5 rounded-pnl border border-line bg-panel p-2 text-ui text-fg">
                      <FillField label="Segments" value={n} min={2} max={64} step={1} onChange={(v) => setSegments({ ...segments, [g.name]: v })} />
                      <div className="flex items-center gap-2">
                        <Chip on={byLength} onClick={() => setByLength(!byLength)} tip="Points as far apart on every guide: long guides get more">By length</Chip>
                        <span className="text-[10.5px] text-muted">{s.tool === "select" ? "the 12 selected guides" : `every guide in ${g.name}`}</span>
                      </div>
                    </Popover.Content>
                  </Popover.Portal>
                </Popover.Root>
                <Glyph tip={shown[g.name] ? "Hide the group" : "Show the group"} onClick={() => setShown({ ...shown, [g.name]: !shown[g.name] })}>
                  {shown[g.name] ? <Eye className="size-3.5" /> : <EyeOff className="size-3.5" />}
                </Glyph>
                <IconMenu tip="Edit Guides: children, mirror, reparent, move, copy…" icon={<MoreHorizontal className="size-3.5" />} items={edits} onPick={(it) => s.say(it.label.includes("per guide") ? `Generated ${it.label} in ${g.name}` : it.label.replace("…", ""))} />
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}

// ---- Paint: what it paints, the brush, the maps, one line each ----------------------------------

function PaintPage() {
  const s = useS();
  const [mode, setMode] = useState("replace");
  const [value, setValue] = useState(1);
  const [radius, setRadius] = useState(60);
  const [strength, setStrength] = useState(1);
  const [pressure, setPressure] = useState(true);
  const [faces, setFaces] = useState(false);
  const [channels, setChannels] = useState({ r: true, g: false, b: false, a: false });
  const [readers, setReaders] = useState<Record<string, string | null>>(Object.fromEntries(maps.map((m) => [m.name, m.readBy])));
  const m = maps.find((x) => x.name === s.map)!;
  const useFor: MenuItem[] = mappable.map((n) => ({ label: n.node, items: n.params.map((p) => ({ label: `${n.node} · ${p}` })) }));
  return (
    <>
      <div className="flex flex-col gap-1.5 rounded-pnl border border-line bg-raised/40 p-2">
        <div className="flex min-w-0 items-center gap-1.5">
          <BrushIcon tool={toolById("paint")} size={18} className="text-accent" />
          <span className="shrink-0 font-semibold">Paint</span>
          <span className="ml-auto flex min-w-0 items-center gap-1 text-sm-ui text-muted">on <span className="truncate text-fg">{m.name}</span>{!readers[m.name] && <span className="shrink-0 text-[10.5px]">· not used yet</span>}</span>
        </div>
        <Seg size="sm" items={paintModes.map((p) => ({ id: p.id, label: p.label, tip: p.tip }))} value={mode} onChange={setMode} />
        {m.color ? (
          <div className="flex items-center gap-1">
            <span className="h-row w-9 shrink-0 rounded-ctl border border-line" style={{ background: "#C8503A" }} />
            {(["r", "g", "b", "a"] as const).map((c) => (
              <Chip key={c} on={channels[c]} onClick={() => setChannels({ ...channels, [c]: !channels[c] })} tip={`Paint changes ${c.toUpperCase()}`}>{c.toUpperCase()}</Chip>
            ))}
            <Button size="sm" className="ml-auto" onClick={() => s.say(`Filled ${m.name}`)}>Fill</Button>
          </div>
        ) : (
          <div className="flex gap-1">
            <FillField label={mode === "clampMaximum" ? "Maximum" : "Value"} value={value} onChange={setValue} className="flex-1" />
            <Button size="sm" onClick={() => s.say(`Filled ${m.name} with ${value.toFixed(2)}`)}>Fill</Button>
          </div>
        )}
        <FillField label="Radius" value={radius} min={1} max={300} step={1} suffix=" px" onChange={setRadius} />
        <FillField label="Strength" value={strength} onChange={setStrength} />
        <div className="flex flex-wrap gap-1">
          <Chip on={s.mirror} onClick={() => s.setMirror(!s.mirror)} tip="Mirror strokes across the head (Alt+Shift+X)">⇆ Mirror</Chip>
          <Chip on={pressure} onClick={() => setPressure(!pressure)}>Pressure</Chip>
          <Chip on={faces} onClick={() => setFaces(!faces)} tip="The brush fills the scalp's faces under it all the way">Whole faces</Chip>
        </div>
        <More label="More settings">
          <FillField label="Falloff" value={0.5} onChange={() => {}} />
          <Row label="Mirror across"><Select value={s.mirrorAxis} options={["World X", "Scalp topology"]} className="flex-1" /></Row>
          <FillField label="Smooth (Shift)" value={0.5} onChange={() => {}} />
          <Row label="Stamp"><Select value="None" options={["None", "Pick image…"]} className="flex-1" /></Row>
          <Row label="Rotation"><Select value="Fixed" options={["Fixed", "Follow Stroke", "Random"]} className="flex-1" /></Row>
          <FillField label="Stamp angle" value={0} min={0} max={360} step={1} suffix="°" onChange={() => {}} />
          <FillField label="Stamp spacing" value={25} min={1} max={200} step={1} suffix="%" onChange={() => {}} />
          <div className="flex flex-wrap gap-1"><Chip on={false} onClick={() => {}}>Invert stamp</Chip><Chip on={false} onClick={() => {}}>Falloff curve</Chip></div>
        </More>
      </div>
      <div className={SECTION}>
        <ListHead title="Maps" count={maps.length}>
          <Glyph tip="New map"><Plus className="size-3.5" /></Glyph>
          <MenuButton label="Import" items={[{ label: "Image…", id: "map.import" }, { label: "From another groom…" }]} className="h-6 px-1.5 text-sm-ui" />
        </ListHead>
        <div className="flex flex-col gap-0.5">
          {maps.map((x) => {
            const reader = readers[x.name];
            const picked = s.map === x.name;
            return (
              // One line per map: its name, then icons. ⓘ holds
              // what reads it (a link into the graph) and Use for; ⋯ holds rename, convert, image and Delete. Qt: item-delegate
              // glyphs on the tree row; ⓘ a small popup QFrame, ⋯ a QToolButton menu.
              <div
                key={x.name}
                onClick={() => s.setMap(x.name)}
                className={cn("flex h-row cursor-default items-center gap-1.5 rounded-ctl pl-1.5", picked ? "bg-selection text-fg" : "text-dim hover:bg-hover")}
              >
                <span className="min-w-0 flex-1 truncate">{x.name}</span>
                {x.color && <span className="shrink-0 rounded-full border border-line px-1 text-[9px] text-muted">RGBA</span>}
                <div className="flex items-center" onClick={(e) => e.stopPropagation()}>
                    <Popover.Root>
                    <Tip label={reader ? `Read by ${reader}` : "Not used by any node yet"}>
                      <Popover.Trigger asChild>
                        <button className="relative flex size-6 shrink-0 items-center justify-center rounded-ctl text-muted outline-none hover:bg-hover hover:text-fg">
                          <Info className="size-3.5" />
                          {!reader && <span className="absolute right-1 top-1 size-1.5 rounded-full bg-accent" />}
                        </button>
                      </Popover.Trigger>
                    </Tip>
                    <Popover.Portal>
                      <Popover.Content align="end" sideOffset={4} className="gl-ui z-50 flex w-64 flex-col gap-1.5 rounded-pnl border border-line bg-panel p-2 text-ui text-fg">
                        <div className="flex items-center gap-1.5"><span className="font-semibold">{x.name}</span><span className="ml-auto text-sm-ui text-muted">{x.kind}</span></div>
                        <Row label="Read by">
                          {reader ? (
                            <button onClick={() => { s.setGraph(true); s.say(`Graph Editor: ${reader.split(" · ")[0]}`); }} className="flex min-w-0 items-center gap-1 truncate text-sm-ui text-accent hover:underline"><Network className="size-3 shrink-0" />{reader}</button>
                          ) : (
                            <span className="text-sm-ui text-muted">nothing yet</span>
                          )}
                        </Row>
                        <MenuButton label={reader ? "Also use for" : "Use for"} items={useFor} onPick={(it) => { setReaders({ ...readers, [x.name]: reader ?? it.label }); s.say(`${it.label} now reads ${x.name}`); }} className={cn("self-start", !reader && "border-accent/60")} />
                        {x.color && <Row label="Scalp shows"><Seg size="sm" className="flex-1" items={["RGB", "R", "G", "B", "A"].map((c) => ({ id: c, label: c }))} value="RGB" onChange={() => {}} /></Row>}
                      </Popover.Content>
                    </Popover.Portal>
                  </Popover.Root>
                  <IconMenu tip="Rename, convert, image, delete" icon={<MoreHorizontal className="size-3.5" />} onPick={(it) => s.say(`${it.label.replace("…", "")}: ${x.name}`)} items={[
                    { label: "Rename…" },
                    { label: `Convert to (now: ${x.kind})`, items: [{ label: "A layer on the guides" }, { label: "Values on the scalp's vertices" }, { label: "A texture at the scalp's UVs…" }] },
                    ...(x.kind.startsWith("Texture") ? [{ label: "Relink Image…" }, { label: "Reload Image" }, { label: "Show File" }] : []),
                    { label: "", sep: true }, { label: "Delete Map", danger: true },
                  ]} />
                </div>
              </div>
            );
          })}
        </div>
      </div>
    </>
  );
}

// ---- Photos: the list is the control -------------------------------------------------------------

/** Clicking a photo looks through its camera (and picks it); clicking it again leaves the camera and
 *  unpicks it. Each row carries its eye and its lock; the picked row opens with how it shows. */
function PhotosPage() {
  const s = useS();
  const [pick, setPick] = useState<string | null>(() => (/[#&]photo=front/.test(window.location.hash) ? photos[0].name : null));
  const [shown, setShown] = useState<Record<string, boolean>>(Object.fromEntries(photos.map((p) => [p.name, true])));
  const [locked, setLocked] = useState<Record<string, boolean>>({ "front.jpg": true });
  const [behind, setBehind] = useState<Record<string, boolean>>({});
  const [opacity, setOpacity] = useState<Record<string, number>>(Object.fromEntries(photos.map((p) => [p.name, 0.5])));
  const [focal, setFocal] = useState<Record<string, number>>(Object.fromEntries(photos.map((p) => [p.name, 50])));
  const click = (name: string) => {
    if (pick === name) { setPick(null); s.say("Back to persp"); }
    else { setPick(name); s.say(`Looking through ${name}`); }
  };
  return (
    <div className={SECTION}>
      <ListHead title="Photos" count={photos.length}>
        <Tip label="Import a photo as a camera"><Button size="sm" variant="ghost" className="h-6 px-1.5 text-sm-ui" onClick={() => s.say("Import Photo")}><Plus className="size-3.5" />Import</Button></Tip>
      </ListHead>
      <div className="flex flex-col gap-0.5">
        {photos.map((p) => {
          const on = pick === p.name;
          const lock = !!locked[p.name];
          return (
            // One line per photo: no expanding. ◐ opens Opacity and Focal length; ⋯ holds Rename,
            // Align Photo (while unlocked), Select Camera and Remove.
            <div
              key={p.name}
              onClick={() => click(p.name)}
              className={cn("flex h-row cursor-default items-center gap-1.5 rounded-ctl pl-1.5", on ? "bg-selection text-fg" : "text-dim hover:bg-hover", !shown[p.name] && "opacity-55")}
            >
              {on ? <Camera className="size-3 shrink-0 text-accent" /> : <span className="w-3 shrink-0" />}
              <span className="min-w-0 flex-1 truncate">{p.name}</span>
              <div className="flex items-center" onClick={(e) => e.stopPropagation()}>
                <Popover.Root>
                  <Tip label={`Opacity ${(opacity[p.name] * 100).toFixed(0)}% · ${focal[p.name]} mm`}>
                    <Popover.Trigger asChild>
                      <button className="flex size-6 shrink-0 items-center justify-center rounded-ctl text-muted outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover"><SlidersHorizontal className="size-3.5" /></button>
                    </Popover.Trigger>
                  </Tip>
                  <Popover.Portal>
                    <Popover.Content align="end" sideOffset={4} className="gl-ui z-50 flex w-60 flex-col gap-1.5 rounded-pnl border border-line bg-panel p-2 text-ui text-fg">
                      <FillField label="Opacity" value={opacity[p.name]} onChange={(v) => setOpacity({ ...opacity, [p.name]: v })} />
                      <div className={cn(lock && "pointer-events-none opacity-45")}><FillField label="Focal length" value={focal[p.name]} min={2.5} max={300} step={0.5} suffix=" mm" onChange={(v) => setFocal({ ...focal, [p.name]: v })} /></div>
                      {lock && <span className="text-[10.5px] text-muted">Locked: unlock to change the lens or align.</span>}
                    </Popover.Content>
                  </Popover.Portal>
                </Popover.Root>
                <Glyph tip={behind[p.name] ? "Behind the mesh: click to draw it in front" : "In front of the mesh: click to draw it behind"} onClick={() => setBehind({ ...behind, [p.name]: !behind[p.name] })}>
                  {behind[p.name] ? <SendToBack className="size-3.5" /> : <BringToFront className="size-3.5" />}
                </Glyph>
                <Glyph tip={shown[p.name] ? "Hide the photo" : "Show the photo"} onClick={() => setShown({ ...shown, [p.name]: !shown[p.name] })}>
                  {shown[p.name] ? <Eye className="size-3.5" /> : <EyeOff className="size-3.5" />}
                </Glyph>
                <Glyph tip={lock ? "Locked: the camera can't move. Click to unlock" : "Lock the camera: position, rotation, focal length"} on={lock} onClick={() => setLocked({ ...locked, [p.name]: !lock })}>
                  {lock ? <Lock className="size-3.5" /> : <LockOpen className="size-3.5" />}
                </Glyph>
                <IconMenu tip="Rename, align, camera, remove" icon={<MoreHorizontal className="size-3.5" />} onPick={(it) => s.say(`${it.label.replace("…", "")}: ${p.name}`)} items={[
                  { label: "Rename…" }, { label: lock ? "Align Photo… (unlock first)" : "Align Photo…" }, { label: "Select Camera" }, { label: "", sep: true }, { label: "Remove Photo", danger: true },
                ]} />
              </div>
            </div>
          );
        })}
      </div>
      <span className="mt-1.5 text-sm-ui text-muted">{pick ? "Click it again to leave the camera." : "Click a photo to look through it."}</span>
    </div>
  );
}

// ---- no groom yet: say what to select, then one button -------------------------------------------

function Empty() {
  const s = useS();
  return (
    <div className="flex flex-col gap-3 rounded-pnl border border-line bg-raised/40 p-3">
      <div>
        <div className="text-[13px] font-semibold">Start a groom</div>
        <div className="text-sm-ui text-dim">Select the scalp mesh (and any guide curves), then create.</div>
      </div>
      <div className={cn("flex items-center gap-2 rounded-ctl border px-2 py-1.5 text-sm-ui", s.meshSelected ? "border-accent/60 text-fg" : "border-dashed border-line text-muted")}>
        <span className={cn("size-2 rounded-full", s.meshSelected ? "bg-accent" : "bg-line")} />
        {s.meshSelected ? "Selected: head_geo (scalp)" : "Nothing selected"}
      </div>
      <Button variant="primary" disabled={!s.meshSelected} onClick={() => { s.setHasGroom(true); s.say("Created groomLabShape1 on head_geo. Place guides to start."); s.setTool("place"); }}>
        Create Groom from Selection
      </Button>
      <div className="flex flex-col gap-1">
        <span className="text-sm-ui text-muted">No mesh yet?</span>
        <div className="flex flex-wrap gap-1">
          {["Nika", "Manny", "Wolf", "Buddy"].map((m) => (
            <Chip key={m} on={false} onClick={() => { s.setMeshSelected(true); s.say(`Added ${m} at the origin, selected`); }}>{m}</Chip>
          ))}
        </div>
      </div>
      <More label="Import XGen, Yeti, Alembic curves or a groom file"><span className="text-sm-ui text-muted">GroomLab ▸ Start From</span></More>
    </div>
  );
}

// ---- the panel -----------------------------------------------------------------------------------

export function GroomTools() {
  const s = useS();
  return (
    <div className="flex h-full flex-col bg-panel text-ui text-fg">
      <div className="flex flex-col gap-2 border-b border-line p-2">
        <Header />
        {s.hasGroom && (
          <Seg size="sm" items={pages} value={s.page} onChange={s.setPage} />
        )}
      </div>
      <div className="gl-scroll flex flex-1 flex-col gap-3 p-2 pb-6">
        {!s.hasGroom ? <Empty /> : s.page === "tools" ? (
          <><Brushes /><ToolCard /><GuideGroups /><Correctives /></>
        ) : s.page === "paint" ? <PaintPage /> : <PhotosPage />}
      </div>
    </div>
  );
}
