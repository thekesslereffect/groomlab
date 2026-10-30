// SPDX-License-Identifier: Apache-2.0
// The Groom Tools content: groom header, tabs, tool palette, brush settings, guide groups.
import { ChevronDown, CircleHelp, Eye, EyeOff, ImagePlus, Key, Link2, MoreHorizontal, Network, Pencil, Plus, Spline, Trash2, X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { FillField, Kbd, Param, Row } from "@/components/ui/field";
import { Section } from "@/components/ui/section";
import { Select } from "@/components/ui/select";
import { TabBar } from "@/components/ui/tabs";
import { Toggle } from "@/components/ui/toggle";
import { Tip } from "@/components/ui/tooltip";
import { allTools, guideGroups, toolGroups, type Tool } from "@/data/groom";
import { icon } from "@/data/icons";
import { gestures, optionLabels, toolSettings, tubeShapes, type ToolSettings } from "@/data/toolSettings";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";
import { useWS } from "./state";
import { useState, type ReactNode } from "react";

export function ToolIcon({ tool, size = 20, className }: { tool: Tool; size?: number; className?: string }) {
  const mono = useOpt("toolIcons") === "mono";
  return mono ? (
    <span className={cn("icon-mono inline-block shrink-0", className)} style={{ width: size, height: size, ["--icon" as string]: `url("${tool.icon}")` }} />
  ) : (
    <img src={tool.icon} width={size} height={size} className={cn("shrink-0", className)} draggable={false} alt="" />
  );
}

export function GroomHeader() {
  const { graphOpen, setGraphOpen } = useWS();
  const [hair, setHair] = useState(true);
  return (
    <div className="flex items-center gap-1">
      <button className="flex h-row min-w-0 flex-1 items-center gap-1.5 rounded-ctl px-1 text-left font-semibold text-fg hover:bg-hover">
        <span className="size-2 shrink-0 rounded-full bg-accent" />
        <span className="truncate">manny_hair</span>
        <ChevronDown className="size-3.5 shrink-0 text-muted" />
      </button>
      <Tip label="Show hair" shortcut="Shift+click: every groom">
        <Button variant="ghost" size="iconSm" onClick={() => setHair(!hair)}>{hair ? <Eye className="size-4" /> : <EyeOff className="size-4" />}</Button>
      </Tip>
      <Tip label="Show guides" shortcut="Alt+Shift+G">
        <Button variant="ghost" size="iconSm"><Spline className="size-4" /></Button>
      </Tip>
      <Tip label="Graph Editor">
        <Button variant={graphOpen ? "checked" : "ghost"} size="iconSm" onClick={() => setGraphOpen(!graphOpen)}><Network className="size-4" /></Button>
      </Tip>
      <Tip label="New groom, theme">
        <Button variant="ghost" size="iconSm"><MoreHorizontal className="size-4" /></Button>
      </Tip>
    </div>
  );
}

export function ToolsTabs() {
  const { toolsTab, setToolsTab } = useWS();
  return (
    <TabBar
      value={toolsTab}
      onChange={setToolsTab}
      items={[
        { id: "tools", label: "Tools" },
        { id: "paint", label: "Paint" },
        { id: "correctives", label: "Correctives" },
        { id: "photos", label: "Photos" },
      ]}
    />
  );
}

function ToolButton({ tool, style }: { tool: Tool; style: string }) {
  const { tool: current, setTool } = useWS();
  const on = current === tool.id;
  if (style === "list") {
    return (
      <button
        onClick={() => tool.id !== "unfreeze" && setTool(tool.id)}
        className={cn("flex h-row w-full items-center gap-2 rounded-ctl px-1.5 text-left outline-none", on ? "bg-selection text-fg" : "text-dim hover:bg-hover hover:text-fg")}
        title={tool.tip}
      >
        <ToolIcon tool={tool} size={16} />
        <span className="flex-1 truncate">{tool.label}</span>
        {on && <span className="size-1.5 rounded-full bg-accent" />}
      </button>
    );
  }
  if (style === "icons") {
    return (
      <Tip label={tool.label} shortcut={tool.tip}>
        <button
          onClick={() => tool.id !== "unfreeze" && setTool(tool.id)}
          className={cn(
            "flex size-[34px] items-center justify-center rounded-ctl border outline-none",
            on ? "border-accent bg-selection text-fg" : "border-transparent text-dim hover:bg-hover hover:text-fg",
          )}
        >
          <ToolIcon tool={tool} size={20} />
        </button>
      </Tip>
    );
  }
  return (
    <button
      onClick={() => tool.id !== "unfreeze" && setTool(tool.id)}
      title={tool.tip}
      className={cn(
        "flex flex-col items-center justify-center gap-1 rounded-ctl border py-1.5 outline-none",
        on ? "border-accent bg-selection text-fg" : "border-line bg-raised text-dim hover:bg-hover hover:text-fg",
      )}
    >
      <ToolIcon tool={tool} size={20} />
      <span className="w-full truncate px-0.5 text-center text-[10px] leading-3">{tool.label}</span>
    </button>
  );
}

export function ToolPalette({ columns }: { columns?: number }) {
  const style = useOpt("tools");
  const cols = columns ?? (style === "icons" ? 6 : style === "tiles" ? 4 : 1);
  return (
    <div className="flex flex-col gap-gap">
      {toolGroups.map((g) => (
        <div key={g.id} className="flex flex-col gap-1">
          <span className="text-sm-ui font-medium text-muted">{g.label}</span>
          {/* Icon tiles keep their size: the panel's width decides how many fit in a row (Qt: ToolGrid
              columns = width / (tile + spacing), tiles fixed, left-aligned). */}
          <div
            className={cn("grid gap-1", style === "list" && "gap-0")}
            style={{ gridTemplateColumns: style === "icons" ? "repeat(auto-fill, 34px)" : style === "tiles" ? "repeat(auto-fill, minmax(60px, 1fr))" : `repeat(${cols}, minmax(0, 1fr))` }}
          >
            {g.tools.map((tool) => <ToolButton key={tool.id} tool={tool} style={style} />)}
          </div>
        </div>
      ))}
    </div>
  );
}

// ---- Small parts -------------------------------------------------------------------------------

/** A toggle chip. Qt: a checkable QToolButton[role="chip"]. */
export function Chip({ on, onClick, children, tip }: { on: boolean; onClick?: () => void; children: ReactNode; tip?: string }) {
  const b = (
    <button
      onClick={onClick}
      className={cn(
        "inline-flex h-[calc(var(--gl-row)-4px)] items-center gap-1 rounded-ctl border px-2 text-sm-ui outline-none",
        on ? "border-transparent bg-selection text-fg" : "border-line text-dim hover:bg-hover hover:text-fg",
      )}
    >
      {children}
    </button>
  );
  return tip ? <Tip label={tip}>{b}</Tip> : b;
}

/** Exclusive chips (one of). Qt: QButtonGroup of chips. */
function Choice({ items, init }: { items: string[]; init?: string }) {
  const [v, setV] = useState(init ?? items[0]);
  return (
    <div className="flex flex-wrap gap-1">
      {items.map((i) => <Chip key={i} on={v === i} onClick={() => setV(i)}>{i}</Chip>)}
    </div>
  );
}

/** Many-of chips. */
function Multi({ items, init }: { items: string[]; init: string[] }) {
  const [v, setV] = useState(init);
  return (
    <div className="flex flex-wrap gap-1">
      {items.map((i) => (
        <Chip key={i} on={v.includes(i)} onClick={() => setV(v.includes(i) ? v.filter((x) => x !== i) : [...v, i])}>{i}</Chip>
      ))}
    </div>
  );
}

/** A row of chips with its name above (chips don't fit a label column). */
function ChipRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex flex-col gap-1 pt-1">
      <span className="text-sm-ui text-muted">{label}</span>
      {children}
    </div>
  );
}

/** A labeled block inside a card: a small muted name, then its rows. */
function Sub({ title, children, actions }: { title: string; children: ReactNode; actions?: ReactNode }) {
  return (
    <div className="flex flex-col gap-0.5 pt-1.5">
      <div className="flex h-5 items-center">
        <span className="flex-1 text-sm-ui font-medium text-muted">{title}</span>
        {actions}
      </div>
      {children}
    </div>
  );
}

function Bool({ label, init = false }: { label: string; init?: boolean }) {
  const [v, setV] = useState(init);
  return <Toggle checked={v} onChange={setV} label={label} />;
}

function Curve({ label }: { label: string }) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="text-sm-ui text-muted">{label}</span>
      <svg viewBox="0 0 100 32" className="h-12 w-full rounded-ctl border border-line bg-raised">
        <path d="M2 4 C 40 4, 55 28, 98 28" fill="none" stroke="var(--gl-accent)" strokeWidth="1.5" />
        <circle cx="2" cy="4" r="2.2" fill="var(--gl-text)" />
        <circle cx="98" cy="28" r="2.2" fill="var(--gl-text)" />
      </svg>
    </div>
  );
}

type ListRow = { name: string; meta?: string; color?: string };

/** A bar of actions over a plain list of rows (the guide groups' look). Qt: QListView + delegate. */
function ListBlock({ rows, actions, empty }: { rows: ListRow[]; actions?: ReactNode; empty?: string }) {
  const [cur, setCur] = useState(rows[0]?.name);
  return (
    <div className="flex flex-col gap-0.5">
      {actions && <div className="flex items-center gap-1">{actions}</div>}
      {rows.length === 0 && <p className="px-2 py-3 text-center text-sm-ui text-muted">{empty}</p>}
      {rows.map((r) => (
        <div
          key={r.name}
          onClick={() => setCur(r.name)}
          className={cn("flex h-row items-center gap-2 rounded-ctl px-1.5", cur === r.name ? "bg-selection" : "hover:bg-hover")}
        >
          {r.color && <span className="size-2.5 shrink-0 rounded-full" style={{ background: r.color }} />}
          <span className="flex-1 truncate text-fg">{r.name}</span>
          {r.meta && <span className="num text-sm-ui text-muted">{r.meta}</span>}
        </div>
      ))}
    </div>
  );
}

const ic = "size-3.5";
function Glyph({ label, children, on, onClick }: { label: string; children: ReactNode; on?: boolean; onClick?: () => void }) {
  return (
    <Tip label={label}>
      <Button variant={on ? "checked" : "ghost"} size="iconSm" onClick={onClick}>{children}</Button>
    </Tip>
  );
}

// ---- The tool card ---------------------------------------------------------------------------

function ToolHelp({ t }: { t: Tool }) {
  return (
    <Tip
      side="left"
      label={
        <div className="flex max-w-64 flex-col gap-1.5 py-0.5">
          <span className="text-fg">{t.tip}</span>
          <div className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5 text-sm-ui">
            {gestures.map(([k, v]) => (
              <span key={k} className="contents">
                <span><Kbd>{k}</Kbd></span>
                <span className="text-dim">{v}</span>
              </span>
            ))}
          </div>
        </div>
      }
    >
      <Button variant="ghost" size="iconSm"><CircleHelp className={ic} /></Button>
    </Tip>
  );
}

function Options({ s }: { s: ToolSettings }) {
  const style = useOpt("gtOptions");
  const keys = s.options ?? [];
  const curves = [s.falloffCurve && "Falloff curve", s.rootToTip && "Root to tip"].filter(Boolean) as string[];
  const [on, setOn] = useState<string[]>(keys.filter((k) => optionLabels[k].on));
  const [curveOn, setCurveOn] = useState<string[]>([]);
  if (!keys.length && !curves.length) return null;
  const flip = (k: string) => setOn(on.includes(k) ? on.filter((x) => x !== k) : [...on, k]);
  const flipCurve = (k: string) => setCurveOn(curveOn.includes(k) ? curveOn.filter((x) => x !== k) : [...curveOn, k]);
  const editors = curveOn.map((c) => <Curve key={c} label={c === "Falloff curve" ? "Falloff: center to edge" : "Strength: root to tip"} />);

  if (style === "grid") {
    const order = ["selectedOnly", "pressure", "collide", "frontFacing", "stretch"] as const;
    return (
      <div className="flex flex-col gap-0.5 pt-1">
        {curves.map((c) => <Toggle key={c} checked={curveOn.includes(c)} onChange={() => flipCurve(c)} label={c} />)}
        {editors}
        <div className="grid grid-cols-2">
          {order.map((k) =>
            keys.includes(k) ? <Toggle key={k} checked={on.includes(k)} onChange={() => flip(k)} label={optionLabels[k].label} /> : <span key={k} />,
          )}
        </div>
      </div>
    );
  }
  const chips = (
    <div className="flex flex-wrap gap-1">
      {keys.map((k) => (
        <Chip key={k} on={on.includes(k)} onClick={() => flip(k)} tip={optionLabels[k].tip}>{optionLabels[k].label}</Chip>
      ))}
      {curves.map((c) => (
        <Chip key={c} on={curveOn.includes(c)} onClick={() => flipCurve(c)} tip={`Shape the ${c.toLowerCase()}`}>{c}</Chip>
      ))}
    </div>
  );
  if (style === "fold") {
    const summary = [...on.map((k) => optionLabels[k].label), ...curveOn].join(" · ") || "none";
    return (
      <Section title="Options" open={false} count={<span className="normal-case tracking-normal">{summary}</span>}>
        {chips}
        {editors}
      </Section>
    );
  }
  return (
    <div className="flex flex-col gap-1 pt-1">
      {chips}
      {editors}
    </div>
  );
}

function TubeFields() {
  return (
    <>
      <Param label="Width" init={0.4} max={2} suffix=" cm" />
      <Param label="Flatness" init={0} />
      <Param label="Twist" init={0} min={-180} max={180} step={1} suffix="°" />
    </>
  );
}

function TubeNew() {
  return (
    <Sub title="Tube">
      <Row label="Shape"><Select value="Round" options={tubeShapes} className="w-full" /></Row>
      <TubeFields />
    </Sub>
  );
}

function TubeEdit() {
  return (
    <Sub title="Selected guides" actions={<Button size="sm" variant="ghost">Smooth</Button>}>
      <div className="flex gap-1">
        <Select value="Round" options={tubeShapes} className="flex-1" />
        <Button size="sm">Apply</Button>
      </div>
      <TubeFields />
    </Sub>
  );
}

/** Today's single Tubes section, for comparison (Tubes: One section). */
function TubesSection() {
  return (
    <Section title="Tubes">
      <p className="text-sm-ui text-muted">No guides selected or soloed: the shape and values here go to the guides Place and Draw make. Select guides to edit theirs.</p>
      <Row label="Shape"><Select value="Round" options={tubeShapes} className="w-full" /></Row>
      <div className="h-28 rounded-ctl border border-line bg-raised" />
      <div className="flex items-center gap-2">
        <Button size="sm">Edit as Custom</Button>
        <div className="flex-1" />
        <Bool label="Mirror" />
        <Bool label="Smooth" />
      </div>
      <div className="flex gap-1">
        <Button size="sm">New</Button>
        <Button size="sm">Rename</Button>
        <Button size="sm">Duplicate</Button>
        <Button size="sm">Delete</Button>
      </div>
      <Button size="sm" disabled>Apply to Selected</Button>
      <TubeFields />
      <div className="flex gap-1">
        <Button size="sm">Smooth</Button>
        <Select value="Current channel" options={["Current channel", "Width", "Flatness", "Twist", "All"]} className="flex-1" />
      </div>
      <Bool label="Fill Tubes" />
      <Param label="Tube Blend" init={50} max={100} step={1} suffix="%" />
    </Section>
  );
}

const paintTool: Tool = {
  id: "paint",
  label: "Paint",
  icon: icon("groomLab_paint"),
  tip: "Paint the current map: Shift smooths, Ctrl paints toward 0. Scalp maps take an alpha stamp.",
};

export function ToolCard({ tool }: { tool?: Tool }) {
  const { tool: current } = useWS();
  const hints = useOpt("gtHints");
  const tubes = useOpt("gtTubes");
  const t = tool ?? allTools.find((x) => x.id === current);
  const s = t && toolSettings[t.id];
  if (!t || !s) return <p className="border-t border-line py-2 text-sm-ui text-muted">Pick a tool, then drag over the groom in the viewport.</p>;
  const split = tubes === "split";
  return (
    <section className="flex flex-col gap-0.5 border-t border-line pt-2">
      <div className="flex h-row items-center gap-2">
        <ToolIcon tool={t} size={16} />
        <span className="flex-1 font-semibold text-fg">{t.label}</span>
        {hints === "short" && <ToolHelp t={t} />}
      </div>
      <p className="pb-1 text-sm-ui leading-snug text-muted">{hints === "short" ? s.short : t.tip}</p>

      {s.brush && (
        <>
          <Param label="Radius" init={60} min={5} max={400} step={1} suffix=" px" />
          <Param label={s.signed ? "Strength (±)" : "Strength"} init={s.signed ? 0.5 : 1} min={s.signed ? -1 : 0} />
          <Param label="Falloff" init={1} />
          {s.smooth && <Param label="Smooth" init={1} max={3} />}
        </>
      )}
      {s.symmetry && (
        <Row label="Symmetry">
          <Toggle checked onChange={() => {}} />
          <Select value="World X" options={["World X", "Topology"]} className="flex-1" />
        </Row>
      )}
      {s.paints && <ChipRow label="Paints"><Choice items={["Width", "Flatness", "Twist"]} /></ChipRow>}
      {s.softRadius && <Param label="Soft radius" init={0.5} max={5} suffix=" cm" />}
      <Options s={s} />
      {s.actsOn && <ChipRow label="Acts on"><Multi items={["Guides", "Parts", "Flow"]} init={["Guides"]} /></ChipRow>}
      {s.erases && (
        <>
          <ChipRow label="Erases"><Multi items={["Guides", "Parts", "Flow"]} init={["Guides"]} /></ChipRow>
          <p className="text-sm-ui text-muted">Erased guides take their children with them.</p>
        </>
      )}
      {s.alpha && (
        <Sub title="Alpha (scalp maps)">
          <Row label="Stamp">
            <span className="h-row w-8 shrink-0 rounded-ctl border border-line" style={{ background: "radial-gradient(circle, var(--gl-text) 0%, transparent 70%)" }} />
            <Select value="Fixed" options={["Fixed", "Follow Stroke", "Random"]} className="flex-1" />
          </Row>
          <Param label="Angle" init={0} min={0} max={360} step={1} suffix="°" />
          <Param label="Spacing" init={25} max={100} step={1} suffix="%" />
        </Sub>
      )}
      {s.newGuides && (
        <Sub title="New guides">
          <Param label="Length" init={8} max={40} step={0.1} suffix=" cm" />
          <Param label="Segments" init={8} min={1} max={32} step={1} />
          {s.newGuides === "place" && <Param label="Spacing" init={1} max={5} step={0.1} suffix=" cm" />}
          <div className="flex flex-wrap gap-1 pt-0.5">
            <Chip on tip="New guides copy the shape of the guides around them">Match nearby</Chip>
            <Chip on={false} tip="Show only the new guides while placing">Solo new</Chip>
          </div>
        </Sub>
      )}
      {split && s.tube === "new" && <TubeNew />}
      {split && s.tube === "edit" && <TubeEdit />}
      {s.parts && (
        <Sub title="Parts" actions={<Glyph label="Parts from curves"><Spline className={ic} /></Glyph>}>
          <Row label="Name">
            <input defaultValue="part" className="h-row w-full rounded-ctl border border-line bg-raised px-2 text-fg outline-none focus:border-accent" />
          </Row>
          <ListBlock rows={[{ name: "center part", meta: "12 pts" }]} />
          <div className="flex gap-1">
            <Button size="sm" variant="ghost">Remove</Button>
            <Button size="sm" variant="ghost">Clear</Button>
          </div>
        </Sub>
      )}
      {s.flow && (
        <Sub title="Flow" actions={<Button size="sm" variant="ghost">Clear all</Button>}>
          <Row label="Place"><Select value="Arrows" options={["Arrows", "Whorl", "Whorl clockwise", "Gather"]} className="w-full" /></Row>
          <p className="text-sm-ui text-muted">No flow arrows yet</p>
        </Sub>
      )}
      {s.lengthKeys && (
        <Sub title="Keys" actions={<Button size="sm" variant="ghost">Clear</Button>}>
          <Param label="Key scale" init={1} max={3} />
          <Param label="Key length" init={8} max={40} suffix=" cm" />
          <ChipRow label="Guides"><Choice items={["Scale", "Trim"]} /></ChipRow>
          <div className="flex gap-1 pt-1">
            <Button size="sm" variant="ghost">Delete key</Button>
            <div className="flex-1" />
            <Button size="sm">Cancel</Button>
            <Button size="sm" variant="primary">Apply</Button>
          </div>
        </Sub>
      )}
      {hints === "full" && s.brush && (
        <p className="pt-1 text-sm-ui leading-snug text-muted">
          B / M + drag (or middle-drag / Shift+middle-drag): radius / strength · Ctrl: the opposite (Length shortens, Lift lays down, Shape shrinks; painting, toward 0) · Shift+drag: paint guides into the selection · Page Up / Down: the brush level · Y + click: every brush.
        </p>
      )}
    </section>
  );
}

/** The current tool's card (Rail and Split layouts use it on its own). */
export function BrushSettings() {
  const tubes = useOpt("gtTubes");
  const { tool } = useWS();
  return (
    <>
      <ToolCard />
      {tubes === "section" && toolSettings[tool]?.tube && <TubesSection />}
    </>
  );
}

// ---- Guide groups ----------------------------------------------------------------------------

export function GuideGroups() {
  const mode = useOpt("gtGroups");
  const [groups, setGroups] = useState(guideGroups);
  const [current, setCurrent] = useState("primary");
  const [iso, setIso] = useState(false);
  return (
    <Section
      title={mode === "list" ? "Guide groups" : "Guide groups & edits"}
      open={mode === "list"}
      count={groups.length}
      actions={
        <span className="flex">
          {mode === "list" && <Glyph label="Isolate the current group" on={iso} onClick={() => setIso(!iso)}><Eye className={ic} /></Glyph>}
          <Glyph label="New group"><Plus className={ic} /></Glyph>
        </span>
      }
    >
      {mode === "combo" && <p className="text-sm-ui text-muted">Place and Draw add to the Group picked at the top; brushes edit it. Guides nodes in the graph read groups by name.</p>}
      {groups.map((g) => (
        <div
          key={g.name}
          onClick={() => setCurrent(g.name)}
          className={cn("group flex h-row items-center gap-2 rounded-ctl px-1.5", current === g.name ? "bg-selection" : "hover:bg-hover")}
        >
          <span className="size-2.5 shrink-0 rounded-full" style={{ background: g.color }} />
          <span className={cn("flex-1 truncate", g.visible ? "text-fg" : "text-muted")}>{g.name}</span>
          <span className="num text-sm-ui text-muted">{g.guides}</span>
          <button
            className="text-muted hover:text-fg"
            onClick={(e) => {
              e.stopPropagation();
              setGroups(groups.map((x) => (x.name === g.name ? { ...x, visible: !x.visible } : x)));
            }}
          >
            {g.visible ? <Eye className={ic} /> : <EyeOff className={ic} />}
          </button>
        </div>
      ))}
      <div className="flex items-center gap-1 pt-1">
        <Button size="sm" variant="ghost">Edit guides<ChevronDown className="size-3" /></Button>
        <div className="flex-1" />
        <Glyph label="Rename"><Pencil className={ic} /></Glyph>
        <Glyph label="Select the group's guides"><Link2 className={ic} /></Glyph>
        <Glyph label="Delete"><Trash2 className={ic} /></Glyph>
      </div>
      <Param label="Segments" init={12} min={2} max={64} step={1} hint="Resample the group's guides" />
    </Section>
  );
}

// ---- Tabs ------------------------------------------------------------------------------------

function PaintTab() {
  return (
    <>
      <Section
        title="Maps"
        count={3}
        actions={
          <span className="flex">
            <Glyph label="Import a map"><ImagePlus className={ic} /></Glyph>
            <Glyph label="New map"><Plus className={ic} /></Glyph>
          </span>
        }
      >
        <ListBlock rows={[{ name: "density", meta: "scalp" }, { name: "clump mask", meta: "guides" }, { name: "tint", meta: "color" }]} />
        <ChipRow label="Mode"><Choice items={["Replace", "Add", "Subtract", "Clamp"]} /></ChipRow>
        <div className="flex gap-1">
          <div className="flex-1"><FillField label="Value" value={1} onChange={() => {}} /></div>
          <Button size="sm">Fill</Button>
        </div>
        <ChipRow label="Show on"><Multi items={["Guides", "Strands", "Scalp"]} init={["Scalp"]} /></ChipRow>
        <p className="text-sm-ui text-muted">Read by scatter.density</p>
      </Section>
      <ToolCard tool={paintTool} />
    </>
  );
}

function CorrectivesTab() {
  return (
    <>
      <Section
        title="Layers"
        count={3}
        actions={
          <span className="flex">
            <Glyph label="Fix this frame: a keyed corrective"><Key className={ic} /></Glyph>
            <Glyph label="New corrective"><Plus className={ic} /></Glyph>
          </span>
        }
      >
        <ListBlock rows={[{ name: "fix frame 24", meta: "0.80 ◆" }, { name: "Node: sculpt1", meta: "1.00" }, { name: "Base groom" }]} />
        <div className="flex gap-1">
          <div className="flex-1"><FillField label="Weight" value={0.8} onChange={() => {}} /></div>
          <Glyph label="Key the weight"><Key className={ic} /></Glyph>
        </div>
        <p className="text-sm-ui text-muted">Brushes from the Tools tab edit the picked layer.</p>
      </Section>
      <ToolCard />
    </>
  );
}

function PhotosTab() {
  return (
    <>
      <Section title="Photos" count={2} actions={<Glyph label="Import a photo"><ImagePlus className={ic} /></Glyph>}>
        <ListBlock rows={[{ name: "front.jpg" }, { name: "side.jpg" }]} />
        <div className="flex flex-wrap gap-1">
          <Chip on>Look through</Chip>
          <Chip on={false}>Lock</Chip>
          <Choice items={["In front", "Behind"]} />
        </div>
        <FillField label="Opacity" value={0.6} onChange={() => {}} />
        <FillField label="Focal length" value={50} min={10} max={200} step={1} suffix=" mm" onChange={() => {}} />
        <Button size="sm">Align photo…</Button>
      </Section>
      <Section title="Guides from photos" open={false}>
        <p className="text-sm-ui text-muted">Experimental: trace guides from two or more aligned photos.</p>
        <div className="flex gap-1">
          <Button size="sm">Open…</Button>
          <Button size="sm">Add camera<ChevronDown className="size-3" /></Button>
        </div>
      </Section>
    </>
  );
}

export function FirstGroomCard({ floating }: { floating?: boolean }) {
  const [gone, setGone] = useState(false);
  if (gone) return null;
  return (
    <div className={cn("rounded-pnl border border-line p-pad", floating ? "w-64 bg-panel" : "bg-raised/50")}>
      <div className="flex items-center">
        <span className="flex-1 font-semibold text-fg">Your first groom</span>
        <button className="text-muted hover:text-fg" onClick={() => setGone(true)}><X className="size-3.5" /></button>
      </div>
      <ol className="mt-1.5 flex flex-col gap-1 text-sm-ui">
        <li className="flex items-center gap-2 text-fg">
          <span className="flex size-4 items-center justify-center rounded-full bg-accent text-[9px] font-bold text-on-accent">1</span>Place some guides
        </li>
        <li className="flex items-center gap-2 text-muted line-through">
          <span className="flex size-4 items-center justify-center rounded-full border border-line text-[9px]">2</span>Give it a look
        </li>
      </ol>
      <p className="mt-1.5 text-sm-ui text-muted">Click the scalp, or drag to lay down many.</p>
    </div>
  );
}

export function OutputButtons({ vertical }: { vertical?: boolean }) {
  return (
    <div className={cn("grid gap-1", vertical ? "grid-cols-3" : "grid-flow-col")}>
      <Button size="sm">Simulate</Button>
      <Button size="sm">Transfer…</Button>
      <Button size="sm">Import<ChevronDown className="size-3" /></Button>
      <Button size="sm">Studio<ChevronDown className="size-3" /></Button>
      <Button size="sm">Render<ChevronDown className="size-3" /></Button>
      <Button size="sm" variant="primary">Export<ChevronDown className="size-3" /></Button>
    </div>
  );
}

export function ToolsPanel({ paletteOnly }: { paletteOnly?: boolean }) {
  const onboarding = useOpt("onboarding");
  const output = useOpt("output");
  const pin = useOpt("gtPin");
  const groups = useOpt("gtGroups");
  const tubes = useOpt("gtTubes");
  const { toolsTab, tool } = useWS();

  const header = (
    <div className="flex flex-col gap-gap">
      <GroomHeader />
      {groups === "combo" && (
        <Row label="Group">
          <Select value="primary" options={guideGroups.map((g) => g.name)} className="flex-1" />
          <Bool label="Isolate" />
        </Row>
      )}
      <ToolsTabs />
    </div>
  );
  const palette = toolsTab === "tools" ? <ToolPalette /> : null;
  const body =
    toolsTab === "tools" ? (
      <>
        {!paletteOnly && <ToolCard />}
        {!paletteOnly && tubes === "section" && toolSettings[tool]?.tube && <TubesSection />}
        <GuideGroups />
      </>
    ) : toolsTab === "paint" ? (
      <PaintTab />
    ) : toolsTab === "correctives" ? (
      <CorrectivesTab />
    ) : (
      <PhotosTab />
    );

  return (
    <div className="flex h-full flex-col">
      {pin !== "none" && (
        <div className="flex flex-col gap-gap p-pad pb-gap">
          {header}
          {pin === "tools" && palette}
        </div>
      )}
      <div className={cn("gl-scroll flex flex-1 flex-col gap-gap px-pad pb-pad", pin === "none" && "pt-pad")}>
        {pin === "none" && header}
        {onboarding === "card" && <FirstGroomCard />}
        {pin !== "tools" && palette}
        {body}
      </div>
      {output === "panel" && (
        <div className="border-t border-line p-pad">
          <OutputButtons vertical />
        </div>
      )}
    </div>
  );
}

export function EmptyTab({ tab }: { tab: string }) {
  return <p className="py-6 text-center text-sm-ui text-muted">{tab}</p>;
}
