// SPDX-License-Identifier: Apache-2.0
// The window: menu bar, then the layout picked in the lab.
// Qt: QMainWindow. Docks = QDockWidgets (title bars hidden: setTitleBarWidget(new QWidget)).
// Rail = a vertical QToolBar of checkable QToolButtons in a QButtonGroup.
// Inset chrome = QMainWindow::setContentsMargins + each dock's content in a rounded QFrame.
import { Network, SlidersHorizontal, Wand2 } from "lucide-react";
import { Fragment, type ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { Tip } from "@/components/ui/tooltip";
import { toolGroups } from "@/data/groom";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";
import { GraphPanel } from "./graph";
import { MenuBar } from "./menubar";
import { PropertiesPanel } from "./properties";
import { useWS } from "./state";
import { BrushSettings, GroomHeader, GuideGroups, OutputButtons, ToolIcon, ToolsPanel, ToolsTabs } from "./tools";
import { Viewport } from "./viewport";

function Dock({ children, className }: { children: ReactNode; className?: string }) {
  const inset = useOpt("chrome") === "inset";
  return <div className={cn("min-h-0 overflow-hidden bg-panel", inset && "rounded-pnl border border-line", className)}>{children}</div>;
}

function Rail() {
  const { tool, setTool, graphOpen, setGraphOpen, railPanel, setRailPanel } = useWS();
  const layout = useOpt("layout");
  return (
    <div className="flex h-full w-11 shrink-0 flex-col items-center py-1.5">
      <div className="gl-scroll flex min-h-0 flex-1 flex-col items-center gap-0.5 [scrollbar-width:none]">
      {toolGroups.map((g, gi) => (
        <Fragment key={g.id}>
          {gi > 0 && <span className="my-1 h-px w-6 bg-line" />}
          {g.tools.map((t) => (
            <Tip key={t.id} label={t.label} shortcut={t.tip} side="right">
              <button
                onClick={() => setTool(t.id)}
                className={cn(
                  "flex size-8 shrink-0 items-center justify-center rounded-ctl outline-none",
                  tool === t.id ? "bg-selection text-fg" : "text-dim hover:bg-hover hover:text-fg",
                )}
              >
                <ToolIcon tool={t} size={18} />
              </button>
            </Tip>
          ))}
        </Fragment>
      ))}
      </div>
      <span className="my-1 h-px w-6 shrink-0 bg-line" />
      {layout === "rail" && (
        <>
          <Tip label="Groom & guide groups" side="right">
            <Button variant={railPanel === "tools" ? "checked" : "ghost"} size="iconSm" className="size-8 shrink-0" onClick={() => setRailPanel("tools")}><Wand2 className="size-4" /></Button>
          </Tip>
          <Tip label="Properties" side="right">
            <Button variant={railPanel === "properties" ? "checked" : "ghost"} size="iconSm" className="size-8 shrink-0" onClick={() => setRailPanel("properties")}><SlidersHorizontal className="size-4" /></Button>
          </Tip>
        </>
      )}
      <Tip label="Graph Editor" side="right">
        <Button variant={graphOpen ? "checked" : "ghost"} size="iconSm" className="size-8 shrink-0" onClick={() => setGraphOpen(!graphOpen)}><Network className="size-4" /></Button>
      </Tip>
    </div>
  );
}

function Center() {
  const { graphOpen, graphMax } = useWS();
  const inset = useOpt("chrome") === "inset";
  const max = graphOpen && graphMax;
  return (
    <div className={cn("flex min-w-0 flex-1 flex-col", inset && "gap-1.5")}>
      {!max && (
        <div className={cn("min-h-0 flex-1 overflow-hidden", inset && "rounded-pnl border border-line")}>
          <Viewport />
        </div>
      )}
      {graphOpen && (
        <Dock className={cn(max ? "flex-1" : "h-[42%] shrink-0", !inset && !max && "border-t border-line")}>
          <GraphPanel />
        </Dock>
      )}
    </div>
  );
}

/** Rail / Split: what the right panel shows besides Properties. */
function GroomPane() {
  const output = useOpt("output");
  return (
    <div className="flex h-full flex-col">
      <div className="flex flex-col gap-gap p-pad pb-gap">
        <GroomHeader />
        <ToolsTabs />
      </div>
      <div className="gl-scroll flex flex-1 flex-col gap-gap px-pad pb-pad">
        <BrushSettings />
        <GuideGroups />
      </div>
      {output === "panel" && <div className="border-t border-line p-pad"><OutputButtons vertical /></div>}
    </div>
  );
}

export function Shell() {
  const layout = useOpt("layout");
  const inset = useOpt("chrome") === "inset";
  const { railPanel } = useWS();
  const seam = !inset;
  return (
    <div className="gl-ui flex h-full flex-col bg-background">
      <MenuBar />
      <div className={cn("flex min-h-0 flex-1", inset && "gap-1.5 p-1.5")}>
        {layout === "docks" && (
          <>
            <Dock className={cn("w-[272px] shrink-0", seam && "border-r border-line")}><ToolsPanel /></Dock>
            <Center />
            <Dock className={cn("w-[320px] shrink-0", seam && "border-l border-line")}><PropertiesPanel /></Dock>
          </>
        )}
        {layout === "rail" && (
          <>
            <Dock className={cn("shrink-0", seam && "border-r border-line")}><Rail /></Dock>
            <Center />
            <Dock className={cn("w-[320px] shrink-0", seam && "border-l border-line")}>
              {railPanel === "tools" ? <GroomPane /> : <PropertiesPanel />}
            </Dock>
          </>
        )}
        {layout === "split" && (
          <>
            <Dock className={cn("shrink-0", seam && "border-r border-line")}><Rail /></Dock>
            <Center />
            <div className={cn("flex w-[330px] shrink-0 flex-col", inset && "gap-1.5", seam && "border-l border-line")}>
              <Dock className={cn("h-[46%] shrink-0", seam && "border-b border-line")}><GroomPane /></Dock>
              <Dock className="flex-1"><PropertiesPanel /></Dock>
            </div>
          </>
        )}
      </div>
    </div>
  );
}
