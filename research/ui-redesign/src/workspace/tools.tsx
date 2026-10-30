// SPDX-License-Identifier: Apache-2.0
// The Groom Tools content: groom header, tabs, tool palette, brush settings, guide groups.
import { ChevronDown, Eye, EyeOff, MoreHorizontal, Network, Plus, Spline, X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Param, Row } from "@/components/ui/field";
import { Section } from "@/components/ui/section";
import { Select } from "@/components/ui/select";
import { TabBar } from "@/components/ui/tabs";
import { Toggle } from "@/components/ui/toggle";
import { Tip } from "@/components/ui/tooltip";
import { allTools, guideGroups, toolGroups, type Tool } from "@/data/groom";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";
import { useWS } from "./state";
import { useState } from "react";

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
        onClick={() => setTool(tool.id)}
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
          onClick={() => setTool(tool.id)}
          className={cn(
            "flex aspect-square items-center justify-center rounded-ctl border outline-none",
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
      onClick={() => setTool(tool.id)}
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
          <div className={cn("grid gap-1", style === "list" && "gap-0")} style={{ gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))` }}>
            {g.tools.map((tool) => <ToolButton key={tool.id} tool={tool} style={style} />)}
          </div>
        </div>
      ))}
    </div>
  );
}

/** The current brush's settings: a section in the docks, a strip over the viewport in Rail. */
export function BrushSettings() {
  const { tool } = useWS();
  const t = allTools.find((x) => x.id === tool) ?? allTools[0];
  const [sym, setSym] = useState(true);
  return (
    <Section title={`${t.label} brush`}>
      <p className="pb-1 text-sm-ui leading-snug text-muted">{t.tip}</p>
      <Param label="Radius" init={0.32} max={2} />
      <Param label="Strength" init={0.6} />
      <Row label="Falloff"><Select value="Smooth" options={["Smooth", "Linear", "Constant"]} className="w-full" /></Row>
      <Row label="Symmetry">
        <Toggle checked={sym} onChange={setSym} />
        <Select value="World X" options={["World X", "Topology", "Object X"]} className="flex-1" />
      </Row>
    </Section>
  );
}

export function GuideGroups() {
  const [groups, setGroups] = useState(guideGroups);
  const [current, setCurrent] = useState("primary");
  return (
    <Section
      title="Guide groups"
      count={groups.length}
      actions={<Button variant="ghost" size="iconSm" title="New group"><Plus className="size-3.5" /></Button>}
    >
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
            {g.visible ? <Eye className="size-3.5" /> : <EyeOff className="size-3.5" />}
          </button>
        </div>
      ))}
    </Section>
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
        <li className="flex items-center gap-2 text-fg"><span className="flex size-4 items-center justify-center rounded-full bg-accent text-[9px] font-bold text-on-accent">1</span>Place some guides</li>
        <li className="flex items-center gap-2 text-muted line-through"><span className="flex size-4 items-center justify-center rounded-full border border-line text-[9px]">2</span>Give it a look</li>
      </ol>
      <p className="mt-1.5 text-sm-ui text-muted">Click the scalp, or drag to lay down many.</p>
    </div>
  );
}

export function OutputButtons({ vertical }: { vertical?: boolean }) {
  return (
    <div className={cn("grid gap-1", vertical ? "grid-cols-2" : "grid-flow-col")}>
      <Button size="sm">Simulate</Button>
      <Button size="sm">Transfer…</Button>
      <Button size="sm">Import <ChevronDown className="size-3" /></Button>
      <Button size="sm">Studio <ChevronDown className="size-3" /></Button>
      <Button size="sm">Render <ChevronDown className="size-3" /></Button>
      <Button size="sm" variant="primary">Export <ChevronDown className="size-3" /></Button>
    </div>
  );
}

export function ToolsPanel({ paletteOnly }: { paletteOnly?: boolean }) {
  const onboarding = useOpt("onboarding");
  const output = useOpt("output");
  const { toolsTab } = useWS();
  return (
    <div className="flex h-full flex-col">
      <div className="flex flex-col gap-gap p-pad pb-gap">
        <GroomHeader />
        <Row label="Group"><Select value="primary" options={guideGroups.map((g) => g.name)} className="w-full" /></Row>
        <ToolsTabs />
      </div>
      <div className="gl-scroll flex flex-1 flex-col gap-gap px-pad pb-pad">
        {onboarding === "card" && <FirstGroomCard />}
        {toolsTab === "tools" ? (
          <>
            <ToolPalette />
            {!paletteOnly && <BrushSettings />}
            <GuideGroups />
          </>
        ) : (
          <EmptyTab tab={toolsTab} />
        )}
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
  const text: Record<string, [string, string]> = {
    paint: ["No maps yet", "Painted maps drive density, length, clumping…"],
    correctives: ["No sculpt layers", "Correctives sculpt on top of the finished groom."],
    photos: ["No reference photos", "Drop photos here to groom against them."],
  };
  const [title, body] = text[tab] ?? ["", ""];
  return (
    <div className="flex flex-col items-center gap-2 rounded-pnl border border-dashed border-line px-pad py-6 text-center">
      <span className="font-medium text-fg">{title}</span>
      <span className="text-sm-ui text-muted">{body}</span>
      <Button size="sm"><Plus className="size-3.5" />Add</Button>
    </div>
  );
}
