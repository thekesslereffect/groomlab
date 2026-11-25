// SPDX-License-Identifier: Apache-2.0
// Find Command: one box over every GroomLab command (engine/ui/actions.json), brush and graph
// preset, so the menus and panels can stay short without hiding anything for good. In Maya: a
// small frameless PySide dialog opened from the Groom Tools header, the GroomLab menu, or a hotkey
// in the GroomLab hotkey set. Qt: QLineEdit over a QListView with a QSortFilterProxyModel.
import { CornerDownLeft, Search } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { cn } from "@/lib/utils";
import { ToolIcon } from "@/workspace/tools";
import { allTools, commandCount, commands, presetCount } from "./data";
import { Key } from "./parts";
import { useS } from "./state";

type Hit = { key: string; label: string; detail: string; kind: string; run: () => void; icon?: React.ReactNode };

function score(q: string, text: string): number {
  if (!q) return 1;
  const t = text.toLowerCase();
  const i = t.indexOf(q);
  if (i === 0) return 3;
  if (i > 0) return t[i - 1] === " " ? 2.5 : 2;
  let k = 0;
  for (const ch of t) if (ch === q[k]) k++;
  return k === q.length ? 1 : 0;
}

export function FindCommand() {
  const s = useS();
  const [q, setQ] = useState("");
  const [sel, setSel] = useState(0);
  const input = useRef<HTMLInputElement>(null);
  useEffect(() => input.current?.focus(), []);

  const all: Hit[] = useMemo(() => [
    ...allTools.filter((t) => !t.action).map((t) => ({
      key: `tool:${t.id}`, label: t.label, detail: t.tip, kind: "Brush", icon: <ToolIcon tool={t} size={16} />,
      run: () => { s.setPage("tools"); s.setTool(t.id); },
    })),
    ...commands.map((c) => ({ key: `cmd:${c.id}`, label: c.label, detail: c.tip, kind: c.kind, run: () => (c.id === "graph.open" ? s.setGraph(true) : s.say(c.label)) })),
  ], [s]);

  const ql = q.trim().toLowerCase();
  const hits = useMemo(() => {
    if (!ql) {
      const start = ["tool:comb", "cmd:file.saveAs", "cmd:export.alembic", "cmd:sim.add", "cmd:graph.open", "cmd:render.turntable"];
      return start.map((k) => all.find((h) => h.key === k)).filter(Boolean) as Hit[];
    }
    return all
      .map((h) => ({ h, v: Math.max(score(ql, h.label) * 2, score(ql, h.detail) * 0.6) }))
      .filter((x) => x.v > 0)
      .sort((a, b) => b.v - a.v)
      .slice(0, 9)
      .map((x) => x.h);
  }, [all, ql]);

  const run = (h?: Hit) => { if (!h) return; h.run(); s.setSearch(false); };
  return (
    <div className="absolute inset-0 z-40 flex items-start justify-center bg-black/30 pt-[14vh]" onPointerDown={(e) => e.target === e.currentTarget && s.setSearch(false)}>
      <div className="gl-ui w-[520px] overflow-hidden rounded-pnl border border-line bg-panel text-ui text-fg">
        <div className="flex items-center gap-2 border-b border-line px-3">
          <Search className="size-4 text-muted" />
          <input
            ref={input}
            value={q}
            onChange={(e) => { setQ(e.target.value); setSel(0); }}
            onKeyDown={(e) => {
              if (e.key === "Escape") s.setSearch(false);
              if (e.key === "ArrowDown") { e.preventDefault(); setSel((v) => Math.min(hits.length - 1, v + 1)); }
              if (e.key === "ArrowUp") { e.preventDefault(); setSel((v) => Math.max(0, v - 1)); }
              if (e.key === "Enter") run(hits[sel]);
            }}
            placeholder="Find a GroomLab command: “alembic”, “comb”, “bake”…"
            className="h-10 flex-1 select-text bg-transparent text-[13px] text-fg outline-none placeholder:text-muted"
          />
          <Key>Esc</Key>
        </div>
        <div className="max-h-[360px] overflow-y-auto p-1.5">
          {!ql && <div className="px-2 pb-1 pt-0.5 text-sm-ui text-muted">Often used</div>}
          {hits.map((h, i) => (
            <button
              key={h.key}
              onPointerMove={() => setSel(i)}
              onClick={() => run(h)}
              className={cn("flex h-10 w-full items-center gap-2.5 rounded-ctl px-2 text-left outline-none", i === sel ? "bg-selection" : "")}
            >
              <span className="flex w-5 justify-center text-muted">{h.icon ?? <span className="size-1.5 rounded-full bg-line" />}</span>
              <span className="min-w-0 flex-1">
                <span className="block truncate text-fg">{h.label}</span>
                <span className="block truncate text-sm-ui text-muted">{h.detail}</span>
              </span>
              <span className="shrink-0 rounded-full border border-line px-1.5 text-[10px] leading-4 text-muted">{h.kind}</span>
              {i === sel && <CornerDownLeft className="size-3.5 text-muted" />}
            </button>
          ))}
          {hits.length === 0 && <div className="px-2 py-6 text-center text-muted">Nothing by that name.</div>}
        </div>
        <div className="flex items-center gap-3 border-t border-line px-3 py-1.5 text-sm-ui text-muted">
          <span><Key>↑</Key> <Key>↓</Key> move</span><span><Key>Enter</Key> run</span>
          <span className="num ml-auto">{commandCount} commands · {allTools.length} brushes · {presetCount} presets</span>
        </div>
      </div>
    </div>
  );
}
