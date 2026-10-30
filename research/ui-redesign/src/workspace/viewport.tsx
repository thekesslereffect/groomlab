// SPDX-License-Identifier: Apache-2.0
// The viewport: a still from the app (public/viewport.png) with the floating UI on top.
// Qt: the QRhi viewport with child QWidgets (the app's OverlayHost) for each floating piece.
import { Camera, Maximize, Redo2, Undo2 } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { FillField } from "@/components/ui/field";
import { Select } from "@/components/ui/select";
import { Toggle } from "@/components/ui/toggle";
import { Tip } from "@/components/ui/tooltip";
import { allTools } from "@/data/groom";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";
import { useWS } from "./state";
import { FirstGroomCard, ToolIcon } from "./tools";

function Float({ className, children }: { className?: string; children: React.ReactNode }) {
  return <div className={cn("absolute flex items-center gap-1 rounded-pnl border border-line bg-panel p-1", className)}>{children}</div>;
}

/** Rail layout: the current brush's settings as a strip along the viewport's top. */
export function BrushStrip() {
  const { tool } = useWS();
  const t = allTools.find((x) => x.id === tool) ?? allTools[0];
  const [r, setR] = useState(0.32);
  const [s, setS] = useState(0.6);
  const [sym, setSym] = useState(true);
  return (
    <Float className="left-1/2 top-2 -translate-x-1/2 gap-2 px-2">
      <ToolIcon tool={t} size={16} />
      <span className="font-semibold text-fg">{t.label}</span>
      <span className="h-4 w-px bg-line" />
      <FillField label="Radius" value={r} max={2} onChange={setR} className="w-36" />
      <FillField label="Strength" value={s} onChange={setS} className="w-36" />
      <Select value="Smooth" options={["Smooth", "Linear", "Constant"]} className="w-24" />
      <span className="h-4 w-px bg-line" />
      <Toggle checked={sym} onChange={setSym} label="Sym X" />
    </Float>
  );
}

export function Viewport() {
  const hud = useOpt("hud");
  const onboarding = useOpt("onboarding");
  const layout = useOpt("layout");
  const [cursor, setCursor] = useState<{ x: number; y: number } | null>(null);
  return (
    <div
      className="relative size-full overflow-hidden bg-viewport"
      onPointerMove={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        setCursor({ x: e.clientX - r.left, y: e.clientY - r.top });
      }}
      onPointerLeave={() => setCursor(null)}
    >
      <img src="/viewport.png" alt="" draggable={false} className="pointer-events-none absolute inset-0 size-full object-cover" />
      {cursor && (
        <div
          className="pointer-events-none absolute size-24 -translate-x-1/2 -translate-y-1/2 rounded-full border-[1.5px] border-white/70"
          style={{ left: cursor.x, top: cursor.y }}
        />
      )}

      {layout === "rail" && <BrushStrip />}

      <Float className="right-2 top-2">
        <Tip label="Undo" shortcut="Ctrl+Z"><Button variant="ghost" size="iconSm"><Undo2 className="size-4" /></Button></Tip>
        <Tip label="Redo" shortcut="Ctrl+Y"><Button variant="ghost" size="iconSm"><Redo2 className="size-4" /></Button></Tip>
      </Float>
      <Float className="right-2 top-14 flex-col">
        <Tip label="Frame all" shortcut="F" side="left"><Button variant="ghost" size="iconSm"><Maximize className="size-4" /></Button></Tip>
        <Tip label="Cameras" side="left"><Button variant="ghost" size="iconSm"><Camera className="size-4" /></Button></Tip>
      </Float>

      {onboarding === "viewport" && (
        <div className="absolute bottom-10 left-3">
          <FirstGroomCard floating />
        </div>
      )}

      {hud !== "hidden" && (
        <div className={cn("absolute bottom-2 left-3 flex items-center gap-2 text-sm-ui", hud === "pills" ? "" : "text-muted")}>
          {hud === "pills" ? (
            <>
              <Chip><span className="size-1.5 rounded-full" style={{ background: "#6DB897" }} />25,843 strands</Chip>
              <Chip>104 guides</Chip>
              <Chip>63 ms / dab</Chip>
              <Chip>GPU</Chip>
            </>
          ) : (
            <span className="num">25,843 strands · 104 guides · 63 ms per dab · GPU</span>
          )}
        </div>
      )}
    </div>
  );
}

function Chip({ children }: { children: React.ReactNode }) {
  return <span className="num flex h-5 items-center gap-1.5 rounded-full border border-line bg-panel px-2 text-dim">{children}</span>;
}
