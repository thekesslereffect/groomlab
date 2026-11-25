// SPDX-License-Identifier: Apache-2.0
// The Simple (Maya-first) mock: GroomLab as it sits inside Maya.
//
//   ┌ Maya menu bar … GroomLab ▾ ───────────────────────────────────────────────────────┐
//   │ status line / shelf (GroomLab tab)                                                │
//   │ tool │ Groom Tools  │ viewport (panel menus, brush ring, HUD)  │ Attribute Editor │
//   │ box  │ (dock, left) ├──────────────────────────────────────────┤ (Maya's own)     │
//   │      │              │ Graph Editor (dock, bottom; when opened) │                  │
//   │ time slider · MEL · Help Line (the active brush's keys)                           │
//   └───────────────────────────────────────────────────────────────────────────────────┘
import { useState } from "react";
import { cn } from "@/lib/utils";
import { GraphPanel } from "@/workspace/graph";
import { ToolIcon } from "@/workspace/tools";
import { brushKeys, toolById, toolHints, tubeHints } from "./data";
import { AttributeEditor, MainMenu, PanelMenus, Shelf, StatusLine, TimeAndHelp, ToolBox } from "./maya";
import { GroomTools } from "./panel";
import { FindCommand } from "./search";
import { useS } from "./state";

function Viewport() {
  const s = useS();
  const [cursor, setCursor] = useState<{ x: number; y: number } | null>(null);
  const tool = toolById(s.tool);
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <PanelMenus />
      <div
        className="relative min-h-0 flex-1 overflow-hidden bg-[#3d4045]"
        onPointerMove={(e) => { const r = e.currentTarget.getBoundingClientRect(); setCursor({ x: e.clientX - r.left, y: e.clientY - r.top }); }}
        onPointerLeave={() => setCursor(null)}
      >
        {s.hasGroom ? (
          <img src="/viewport.png" alt="" draggable={false} className="pointer-events-none absolute inset-0 size-full object-cover" />
        ) : (
          <div className="absolute inset-0 bg-gradient-to-b from-[#5a6068] to-[#2c2f33]" />
        )}
        {/* the brush: its ring and name at the cursor (the plugin's draw override already draws these) */}
        {cursor && s.hasGroom && (
          <div className="pointer-events-none absolute" style={{ left: cursor.x, top: cursor.y }}>
            <div className="absolute size-[120px] -translate-x-1/2 -translate-y-1/2 rounded-full border-[1.5px] border-[#e8e8e8]/80" />
            <div className="absolute left-[66px] top-1 flex items-center gap-1.5 whitespace-nowrap rounded-[3px] bg-black/50 px-1.5 py-0.5 text-[11px] text-white/90">
              <ToolIcon tool={tool} size={13} />{tool.label}
            </div>
          </div>
        )}
        <div className="absolute bottom-2 left-1/2 -translate-x-1/2 font-[Segoe_UI] text-[12px] text-[#e5e5e5]">persp</div>
        <div className="num absolute left-2 top-2 font-[Segoe_UI] text-[11px] leading-4 text-[#e5e5e5]/80">
          {s.hasGroom ? <>Strands: 25,843<br />Guides: 104</> : null}
        </div>
        {/* Maya's in-view message: what just happened */}
        {s.toast && (
          <div key={s.toast.id} className="absolute left-1/2 top-[18%] -translate-x-1/2 whitespace-nowrap rounded-[3px] bg-black/60 px-3 py-1.5 font-[Segoe_UI] text-[12px] text-[#f0f0f0]">
            {s.toast.text}
          </div>
        )}
      </div>
    </div>
  );
}

export function SimpleShell() {
  const s = useS();
  const hint = s.tool === "shape" ? tubeHints[s.tubeMode === "roots" ? s.rootMode : s.tubeMode] : toolHints[s.tool] ?? "";
  const help = s.hasGroom ? `${toolById(s.tool).label}: ${hint}   ·   ${brushKeys}` : "Select the scalp mesh, then GroomLab ▸ Create Groom from Selection.";
  return (
    <div className="relative flex h-full flex-col bg-[#444444]">
      <MainMenu />
      <StatusLine />
      <Shelf />
      <div className="flex min-h-0 flex-1 border-t border-[#2b2b2b]">
        <ToolBox />
        <div className="gl-ui w-[320px] shrink-0 border-r border-[#2b2b2b]"><GroomTools /></div>
        <div className="flex min-w-0 flex-1 flex-col">
          {!(s.graph && s.graphMax) && <Viewport />}
          {s.graph && (
            <div className={cn("gl-ui shrink-0 border-t-4 border-[#444444] bg-panel", s.graphMax ? "flex-1" : "h-[42%]")}>
              <GraphPanel />
            </div>
          )}
        </div>
        <AttributeEditor />
      </div>
      <TimeAndHelp help={help} />
      {s.search && <FindCommand />}
    </div>
  );
}
