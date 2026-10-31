// SPDX-License-Identifier: Apache-2.0
// The Graph Editor dock. Qt: the app's node_graph.cpp and Maya's graph_editor.py paint the nodes
// with QPainter, so every option under Graph is a drawing choice (theme graph.* keys), not QSS.
import { Ban, ChevronDown, Eye, Focus, Magnet, Maximize2, Minimize2, MoreHorizontal, Plus, Search, TriangleAlert, X } from "lucide-react";
import { useCallback, useEffect, useLayoutEffect, useRef, useState, type CSSProperties } from "react";
import { Button } from "@/components/ui/button";
import { Param, Row } from "@/components/ui/field";
import { Section } from "@/components/ui/section";
import { Select } from "@/components/ui/select";
import { Tip } from "@/components/ui/tooltip";
import { graphCategories, graphNodes, graphWires, portColors, type GNode } from "@/data/groom";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";
import { useWS } from "./state";

// The theme's state colors (charcoal.json graph.* and palette), as the hosts paint them.
const FOCUS = "#7C9CD4";
const WARNING = "#D4B26A";
const SLOW_MS = 10;

const HEAD = 26;
const PORT = 18;

type Geo = { w: number; h: number; ins: number[]; outs: number[]; valuesTop: number };

/** Where a node's ports sit (y from the node's top), for the card and the wires alike. */
function geometry(n: GNode, body: string): Geo {
  if (body === "compact") {
    const h = 30;
    const spread = (k: number) => Array.from({ length: k }, (_, i) => (h * (i + 1)) / (k + 1));
    return { w: 132, h, ins: spread(n.ins.length), outs: spread(n.outs.length), valuesTop: h };
  }
  const rows = Math.max(n.ins.length, n.outs.length);
  const top = HEAD + 6;
  const at = (k: number) => Array.from({ length: k }, (_, i) => top + i * PORT + PORT / 2);
  const valuesTop = top + rows * PORT + (rows ? 2 : 0);
  const vals = body === "values" ? (n.values?.length ?? 0) : 0;
  return { w: 152, h: valuesTop + (vals ? vals * PORT + 8 : 4), ins: at(n.ins.length), outs: at(n.outs.length), valuesTop };
}

function readableOn(hex: string) {
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b > 0.55 ? "#1C1D21" : "#FFFFFF";
}

function wirePath(x1: number, y1: number, x2: number, y2: number, style: string) {
  if (style === "straight") return `M${x1},${y1} L${x2},${y2}`;
  if (style === "angled") {
    const xm = x1 + Math.max(16, (x2 - x1) / 2);
    const dy = y2 - y1;
    const r = Math.min(8, Math.abs(dy) / 2, Math.abs(xm - x1));
    if (r < 0.5) return `M${x1},${y1} H${x2}`;
    const s = Math.sign(dy);
    return `M${x1},${y1} H${xm - r} Q${xm},${y1} ${xm},${y1 + s * r} V${y2 - s * r} Q${xm},${y2} ${xm + r},${y2} H${x2}`;
  }
  const dx = Math.max(40, (x2 - x1) / 2);
  return `M${x1},${y1} C${x1 + dx},${y1} ${x2 - dx},${y2} ${x2},${y2}`;
}

export function GraphPanel() {
  const { setGraphOpen, graphMax, setGraphMax } = useWS();
  const body = useOpt("nodeBody");
  const wires = useOpt("wires");
  const wireColor = useOpt("wireColor");
  const canvas = useOpt("canvas");
  const [sel, setSel] = useState("clump");
  const [pan, setPan] = useState({ x: 16, y: 16 });
  const [zoom, setZoom] = useState(1);
  const view = useRef<HTMLDivElement>(null);
  const node = graphNodes.find((n) => n.id === sel);

  // Opening (and resizing) keeps the nodes readable: at least 75%, from the top left when the graph
  // is wider than the view. Frame all (F, the button) fits everything, however small.
  const frame = useCallback((all = false) => {
    const el = view.current;
    if (!el || el.clientWidth < 80 || el.clientHeight < 80) return;
    const boxes = graphNodes.map((n) => ({ n, g: geometry(n, body) }));
    const x0 = Math.min(...boxes.map((b) => b.n.x)), y0 = Math.min(...boxes.map((b) => b.n.y));
    const x1 = Math.max(...boxes.map((b) => b.n.x + b.g.w)), y1 = Math.max(...boxes.map((b) => b.n.y + b.g.h));
    const pad = 32;
    const fit = Math.min(1.25, (el.clientWidth - pad * 2) / (x1 - x0), (el.clientHeight - pad * 2) / (y1 - y0));
    const z = Math.max(all ? 0.4 : 0.75, fit);
    const cx = (el.clientWidth - (x1 - x0) * z) / 2, cy = (el.clientHeight - (y1 - y0) * z) / 2;
    setZoom(z);
    setPan({ x: (all ? cx : Math.max(pad, cx)) - x0 * z, y: (all ? cy : Math.min(pad * 2, Math.max(pad, cy))) - y0 * z });
  }, [body]);
  useLayoutEffect(() => frame(), [frame]);
  useEffect(() => {
    const fit = () => frame();
    const ro = new ResizeObserver(fit);
    window.addEventListener("resize", fit);
    if (view.current) ro.observe(view.current);
    const onKey = (e: KeyboardEvent) => {
      if ((e.key === "f" || e.key === "F") && !(e.target as HTMLElement).closest("input")) frame(true);
    };
    window.addEventListener("keydown", onKey);
    return () => { ro.disconnect(); window.removeEventListener("resize", fit); window.removeEventListener("keydown", onKey); };
  }, [frame]);
  const byId = Object.fromEntries(graphNodes.map((n) => [n.id, n]));

  const grid = 20 * zoom;
  const canvasStyle: CSSProperties =
    canvas === "dots"
      ? { backgroundImage: "radial-gradient(circle, var(--gl-border) 1px, transparent 1.2px)", backgroundSize: `${grid}px ${grid}px` }
      : canvas === "lines"
        ? {
            backgroundImage:
              "linear-gradient(var(--gl-border) 1px, transparent 1px), linear-gradient(90deg, var(--gl-border) 1px, transparent 1px)",
            backgroundSize: `${grid}px ${grid}px`,
          }
        : {};

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(var(--gl-row)+8px)] shrink-0 items-center gap-1 border-b border-line px-pad">
        <span className="font-semibold text-fg">Graph</span>
        <span className="num mr-2 text-sm-ui text-muted">{graphNodes.length} nodes</span>
        <Button size="sm" variant="ghost"><Plus className="size-3.5" />Add node</Button>
        <Button size="sm" variant="ghost">Presets<ChevronDown className="size-3" /></Button>
        <div className="flex-1" />
        <div className="flex h-[calc(var(--gl-row)-4px)] w-44 items-center gap-1.5 rounded-ctl border border-line bg-raised px-2 text-sm-ui text-muted">
          <Search className="size-3.5" />Find node…
        </div>
        <span className="num w-10 text-right text-sm-ui text-muted">{Math.round(zoom * 100)}%</span>
        <Tip label="Frame all" shortcut="F"><Button size="iconSm" variant="ghost" onClick={() => frame(true)}><Focus className="size-3.5" /></Button></Tip>
        <Tip label="Snap"><Button size="iconSm" variant="ghost"><Magnet className="size-3.5" /></Button></Tip>
        <Tip label={graphMax ? "Restore" : "Maximize"}>
          <Button size="iconSm" variant="ghost" onClick={() => setGraphMax(!graphMax)}>
            {graphMax ? <Minimize2 className="size-3.5" /> : <Maximize2 className="size-3.5" />}
          </Button>
        </Tip>
        <Button size="iconSm" variant="ghost"><MoreHorizontal className="size-3.5" /></Button>
        <Button size="iconSm" variant="ghost" onClick={() => { setGraphOpen(false); setGraphMax(false); }}><X className="size-3.5" /></Button>
      </div>
      <div className="flex min-h-0 flex-1">
        <div
          ref={view}
          className="relative flex-1 overflow-hidden bg-background"
          style={{ ...canvasStyle, backgroundPosition: `${pan.x}px ${pan.y}px` }}
          onPointerDown={(e) => {
            if (e.target !== e.currentTarget) return;
            (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
            setSel("");
          }}
          onPointerMove={(e) => e.buttons === 1 && e.target === e.currentTarget && setPan((p) => ({ x: p.x + e.movementX, y: p.y + e.movementY }))}
          onWheel={(e) => {
            const r = e.currentTarget.getBoundingClientRect();
            const mx = e.clientX - r.left, my = e.clientY - r.top;
            const z = Math.max(0.4, Math.min(2, zoom * (e.deltaY < 0 ? 1.1 : 1 / 1.1)));
            setPan((p) => ({ x: mx - ((mx - p.x) * z) / zoom, y: my - ((my - p.y) * z) / zoom }));
            setZoom(z);
          }}
        >
          <div className="pointer-events-none absolute left-0 top-0 origin-top-left" style={{ transform: `translate(${pan.x}px,${pan.y}px) scale(${zoom})` }}>
            <svg className="absolute overflow-visible" width={1} height={1}>
              {graphWires.map(([a, ai, b, bi], i) => {
                const na = byId[a], nb = byId[b];
                const ga = geometry(na, body), gb = geometry(nb, body);
                const lit = sel === a || sel === b;
                const typeColor = portColors[na.outs[ai]] ?? "#888";
                const color = wireColor === "neutral" ? (lit ? "var(--gl-text)" : "var(--gl-muted)") : typeColor;
                const dashed = na.bypass || nb.bypass;
                return (
                  <path
                    key={i}
                    d={wirePath(na.x + ga.w, na.y + ga.outs[ai], nb.x, nb.y + gb.ins[bi], wires)}
                    stroke={color}
                    strokeOpacity={wireColor === "neutral" ? (lit ? 1 : 0.6) : lit ? 1 : 0.6}
                    strokeWidth={lit ? 2 : 1.5}
                    strokeDasharray={dashed ? "4 3" : undefined}
                    fill="none"
                  />
                );
              })}
            </svg>
            {graphNodes.map((n) => <NodeCard key={n.id} n={n} selected={sel === n.id} onSelect={() => setSel(n.id)} />)}
          </div>
        </div>
        <div className="gl-scroll w-72 shrink-0 border-l border-line bg-panel p-pad">
          {node ? (
            <div className="flex flex-col gap-gap">
              <div className="flex items-center gap-2">
                <span className="size-2.5 rounded-sm" style={{ background: graphCategories[node.cat] }} />
                <span className="flex-1 font-semibold text-fg">{node.label}</span>
                <span className="text-sm-ui text-muted">{node.cat}</span>
              </div>
              {node.warning && (
                <div className="flex gap-2 rounded-ctl border border-line px-2 py-1.5 text-sm-ui text-dim">
                  <TriangleAlert className="mt-px size-3.5 shrink-0" style={{ color: WARNING }} />
                  {node.warning}
                </div>
              )}
              <Section title="Settings">
                <Param label="Strength" init={0.8} />
                <Param label="Clump Size" init={0.35} />
                <Param label="Tip Spread" init={0.2} />
                <Row label="Profile"><Select value="Ease In" options={["Linear", "Ease In", "Ease Out"]} className="w-full" /></Row>
              </Section>
              <Section title="Randomness" open={false}>
                <Param label="Strength Random" init={0.1} />
              </Section>
            </div>
          ) : (
            <p className="py-6 text-center text-sm-ui text-muted">Select a node to edit it.</p>
          )}
        </div>
      </div>
    </div>
  );
}

function Port({ type, side, y }: { type: string; side: "in" | "out"; y: number }) {
  const shape = useOpt("ports");
  const color = portColors[type] ?? "#888";
  const base: CSSProperties = { top: y, [side === "in" ? "left" : "right"]: 0, transform: `translate(${side === "in" ? "-50%" : "50%"}, -50%)` };
  if (shape === "rings") {
    return <span className="absolute size-[10px] rounded-full bg-panel" style={{ ...base, border: `2px solid ${color}` }} />;
  }
  if (shape === "shapes") {
    const kind = type === "guides" ? "diamond" : type === "mesh" ? "square" : "circle";
    return (
      <span
        className={cn("absolute border-[1.5px] border-panel", kind === "circle" ? "size-[10px] rounded-full" : "size-[9px] rounded-[2px]")}
        style={{ ...base, background: color, transform: `${base.transform}${kind === "diamond" ? " rotate(45deg)" : ""}` }}
      />
    );
  }
  return <span className="absolute size-[9px] rounded-full border-[1.5px] border-panel" style={{ ...base, background: color }} />;
}

function Badges({ n, onFilled }: { n: GNode; onFilled?: string }) {
  if (useOpt("nodeStatus") !== "badges") return null;
  const tone = onFilled ? { color: onFilled, opacity: 0.75 } : undefined;
  return (
    <span className="ml-auto flex shrink-0 items-center gap-1">
      {n.warning && <TriangleAlert className="size-3" style={onFilled ? tone : { color: WARNING }} />}
      {n.bypass && <Ban className="size-3" style={tone} />}
      {n.view && <Eye className="size-3" style={onFilled ? tone : { color: FOCUS }} />}
      {n.ms != null && (
        <span className="num text-[10px]" style={onFilled ? tone : { color: n.ms >= SLOW_MS ? WARNING : "var(--gl-muted)" }}>
          {n.ms < 1 ? n.ms.toFixed(1) : n.ms.toFixed(n.ms < 10 ? 1 : 0)} ms
        </span>
      )}
    </span>
  );
}

function NodeCard({ n, selected, onSelect }: { n: GNode; selected: boolean; onSelect: () => void }) {
  const header = useOpt("nodeHeader");
  const body = useOpt("nodeBody");
  const g = geometry(n, body);
  const color = graphCategories[n.cat];
  const compact = body === "compact";
  const filled = header === "filled";
  const ink = filled ? readableOn(color) : undefined;

  const titleRow = (
    <div
      className={cn("flex items-center gap-1.5 px-2", !compact && (header === "tinted" || filled) && "border-b border-line")}
      style={{
        height: compact ? g.h - 2 : HEAD,
        paddingLeft: header === "strip" ? 11 : undefined,
        background: filled ? color : header === "tinted" ? `color-mix(in srgb, ${color} 16%, transparent)` : undefined,
        borderBottomColor: filled ? "transparent" : undefined,
      }}
    >
      {(header === "tinted" || header === "minimal") && <span className="size-2 shrink-0 rounded-full" style={{ background: color }} />}
      <span className={cn("truncate text-sm-ui font-semibold", n.bypass && "line-through")} style={{ color: ink ?? "var(--gl-text)" }}>{n.label}</span>
      <Badges n={n} onFilled={ink} />
    </div>
  );

  return (
    <div
      onPointerDown={onSelect}
      title={compact ? `${n.label}: ${[...n.ins.map((p) => `in ${p}`), ...n.outs.map((p) => `out ${p}`)].join(", ")}` : undefined}
      className="pointer-events-auto absolute"
      style={{ left: n.x, top: n.y, width: g.w, height: g.h, opacity: n.bypass ? 0.5 : 1 }}
    >
      <div
        className="relative size-full overflow-hidden rounded-pnl border bg-panel"
        style={{
          borderColor: selected ? "var(--gl-text)" : "var(--gl-border)",
          outline: selected ? "1px solid var(--gl-text)" : undefined,
          outlineOffset: 1,
          background: filled && compact ? color : undefined,
        }}
      >
        {header === "strip" && <span className="absolute inset-y-0 left-0 w-[3px]" style={{ background: color }} />}
        {titleRow}
        {!compact && (
          <>
            {n.ins.map((p, i) => (
              <span key={p} className="absolute left-2.5 -translate-y-1/2 text-[10px] text-dim" style={{ top: g.ins[i] }}>{p}</span>
            ))}
            {n.outs.map((p, i) => (
              <span key={p} className="absolute right-2.5 -translate-y-1/2 text-[10px] text-dim" style={{ top: g.outs[i] }}>{p}</span>
            ))}
            {body === "values" && n.values && (
              <div className="absolute inset-x-0 border-t border-line px-2.5 pt-1" style={{ top: g.valuesTop }}>
                {n.values.map(([k, v]) => (
                  <div key={k} className="flex items-center text-[10px]" style={{ height: PORT }}>
                    <span className="flex-1 truncate text-muted">{k}</span>
                    <span className="num text-dim">{v}</span>
                  </div>
                ))}
              </div>
            )}
          </>
        )}
      </div>
      {n.ins.map((p, i) => <Port key={`i${p}`} type={p} side="in" y={g.ins[i]} />)}
      {n.outs.map((p, i) => <Port key={`o${p}`} type={p} side="out" y={g.outs[i]} />)}
    </div>
  );
}
