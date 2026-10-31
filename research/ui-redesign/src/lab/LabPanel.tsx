// SPDX-License-Identifier: Apache-2.0
// The design switcher. Styled on its own (fixed zinc colors, not the tokens) so it stays put
// while the design under it changes. ` toggles it, X flips A/B, ←/→ on a focused row cycles it.
import { useEffect, useState } from "react";
import { cn } from "@/lib/utils";
import { defaults, dimensions, directions, type DimId } from "./options";
import { useLab } from "./store";

export type Page = "workspace" | "components" | "export";

export function LabPanel({ page, setPage }: { page: Page; setPage: (p: Page) => void }) {
  const lab = useLab();
  const [open, setOpen] = useState(() => !/(^|[#&])lab=0/.test(window.location.hash));
  const [name, setName] = useState("");
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.target as HTMLElement).tagName === "INPUT") return;
      if (e.key === "`") setOpen((o) => !o);
      if (e.key === "x" || e.key === "X") lab.flipAB();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [lab]);

  const groups = [...new Set(dimensions.map((d) => d.group))];
  const activeDirection = directions.find((d) => Object.entries(d.options).every(([k, v]) => lab.options[k as DimId] === v));

  if (!open) {
    return (
      <button
        onClick={() => setOpen(true)}
        className="fixed bottom-3 right-3 z-[100] rounded-full bg-zinc-100 px-3 py-1.5 font-[Inter,sans-serif] text-xs font-semibold text-zinc-900 shadow-lg"
      >
        Design lab <span className="text-zinc-500">`</span>
      </button>
    );
  }

  return (
    <aside className="flex h-full w-[340px] shrink-0 flex-col overflow-hidden border-l border-zinc-800 bg-zinc-950 font-[Inter,sans-serif] text-[12px] text-zinc-200">
      <div className="flex items-center gap-2 border-b border-zinc-800 px-3 py-2">
        <span className="whitespace-nowrap font-semibold text-white">Design lab</span>
        <div className="ml-auto flex rounded-md bg-zinc-900 p-0.5">
          {(["workspace", "components", "export"] as Page[]).map((p) => (
            <button key={p} onClick={() => setPage(p)} className={cn("whitespace-nowrap rounded px-2 py-0.5 capitalize", page === p ? "bg-zinc-700 text-white" : "text-zinc-400 hover:text-white")}>
              {p === "export" ? "Qt" : p === "components" ? "Parts" : "App"}
            </button>
          ))}
        </div>
        <button onClick={() => setOpen(false)} className="text-zinc-500 hover:text-white" title="Hide (`)">✕</button>
      </div>

      <div className="flex-1 overflow-y-auto px-3 py-2 [scrollbar-width:thin]">
        <Label>Directions</Label>
        <div className="mb-3 grid grid-cols-2 gap-1">
          {directions.map((d) => (
            <button
              key={d.id}
              onClick={() => lab.apply(d.options)}
              title={d.hint}
              className={cn("rounded-md border px-2 py-1.5 text-left", activeDirection?.id === d.id ? "border-zinc-300 bg-zinc-800 text-white" : "border-zinc-800 hover:border-zinc-600")}
            >
              <div className="font-medium">{d.label}</div>
              <div className="truncate text-[10.5px] text-zinc-500">{d.hint}</div>
            </button>
          ))}
        </div>

        {groups.map((g) => (
          <div key={g} className="mb-3">
            <Label>{g}</Label>
            <div className="flex flex-col gap-1.5">
              {dimensions.filter((d) => d.group === g).map((d) => (
                <div
                  key={d.id}
                  tabIndex={0}
                  onKeyDown={(e) => {
                    if (e.key === "ArrowRight") lab.cycle(d.id, 1);
                    if (e.key === "ArrowLeft") lab.cycle(d.id, -1);
                  }}
                  className="rounded-md px-1 py-0.5 outline-none focus:bg-zinc-900"
                  title={"qt" in d ? `Qt: ${d.qt}` : undefined}
                >
                  <div className="mb-0.5 flex items-center text-zinc-400">
                    {d.label}
                    {lab.options[d.id] !== defaults[d.id] && <span className="ml-1 size-1 rounded-full bg-sky-400" />}
                  </div>
                  <div className="flex flex-wrap gap-1">
                    {d.choices.map((c) => (
                      <button
                        key={c.id}
                        onClick={() => lab.set(d.id, c.id)}
                        title={"hint" in c ? c.hint : undefined}
                        className={cn(
                          "rounded border px-1.5 py-0.5",
                          lab.options[d.id] === c.id ? "border-zinc-200 bg-zinc-100 text-zinc-900" : "border-zinc-800 text-zinc-300 hover:border-zinc-600",
                        )}
                      >
                        {c.label}
                      </button>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          </div>
        ))}

        <Label>A / B</Label>
        <div className="mb-3 flex items-center gap-1">
          <Small onClick={() => lab.storeSlot("a")} on={lab.ab.showing === "a"}>{lab.ab.a ? "Store A" : "Set A"}</Small>
          <Small onClick={() => lab.storeSlot("b")} on={lab.ab.showing === "b"}>{lab.ab.b ? "Store B" : "Set B"}</Small>
          <Small onClick={lab.flipAB} disabled={!lab.ab.a || !lab.ab.b}>Flip <span className="text-zinc-500">X</span></Small>
          <span className="ml-1 text-[10.5px] text-zinc-500">{lab.ab.showing ? `showing ${lab.ab.showing.toUpperCase()}` : ""}</span>
        </div>

        <Label>Snapshots</Label>
        <div className="mb-1 flex gap-1">
          <input
            value={name}
            onChange={(e) => setName(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter" && name.trim()) {
                lab.saveSnapshot(name.trim());
                setName("");
              }
            }}
            placeholder="Name this combo…"
            className="min-w-0 flex-1 rounded border border-zinc-800 bg-zinc-900 px-2 py-1 outline-none focus:border-zinc-500"
          />
          <Small onClick={() => { if (name.trim()) { lab.saveSnapshot(name.trim()); setName(""); } }}>Save</Small>
        </div>
        <div className="mb-3 flex flex-col">
          {lab.snapshots.length === 0 && <span className="text-[10.5px] text-zinc-500">None yet. Saved in this browser.</span>}
          {lab.snapshots.map((s) => (
            <div key={s.name} className="group flex items-center gap-2 rounded px-1 py-0.5 hover:bg-zinc-900">
              <button className="flex-1 truncate text-left" onClick={() => lab.apply(s.options)}>{s.name}</button>
              <button className="text-zinc-600 opacity-0 hover:text-red-400 group-hover:opacity-100" onClick={() => lab.removeSnapshot(s.name)}>✕</button>
            </div>
          ))}
        </div>
      </div>

      <div className="flex gap-1 border-t border-zinc-800 px-3 py-2">
        <Small
          onClick={() => {
            navigator.clipboard?.writeText(window.location.href);
            setCopied(true);
            setTimeout(() => setCopied(false), 1200);
          }}
        >
          {copied ? "Copied" : "Copy link"}
        </Small>
        <Small
          onClick={() =>
            lab.apply(Object.fromEntries(dimensions.map((d) => [d.id, d.choices[Math.floor(Math.random() * d.choices.length)].id])))
          }
        >
          Shuffle
        </Small>
        <Small onClick={() => lab.apply(defaults)}>Reset</Small>
      </div>
    </aside>
  );
}

function Label({ children }: { children: React.ReactNode }) {
  return <div className="mb-1 text-[10px] font-semibold uppercase tracking-wider text-zinc-500">{children}</div>;
}

function Small({ children, onClick, on, disabled }: { children: React.ReactNode; onClick: () => void; on?: boolean; disabled?: boolean }) {
  return (
    <button
      disabled={disabled}
      onClick={onClick}
      className={cn("rounded border px-2 py-1 disabled:opacity-40", on ? "border-zinc-300 bg-zinc-800 text-white" : "border-zinc-800 hover:border-zinc-600")}
    >
      {children}
    </button>
  );
}
