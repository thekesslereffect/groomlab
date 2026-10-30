// SPDX-License-Identifier: Apache-2.0
// The chosen options: in the URL hash (shareable links, only what differs from the defaults)
// and in localStorage (survives reloads). Snapshots and the A/B slots live in localStorage too.
import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from "react";
import { defaults, dimensions, directions, type DimId, type Options } from "./options";

const KEY = "gl-lab-options";
const SNAP_KEY = "gl-lab-snapshots";
const AB_KEY = "gl-lab-ab";

export type Snapshot = { name: string; options: Options; note?: string };

function valid(o: Partial<Record<string, string>>): Options {
  const out = { ...defaults };
  for (const d of dimensions) {
    const v = o[d.id];
    if (v && d.choices.some((c) => c.id === v)) out[d.id as DimId] = v;
  }
  return out;
}

function fromHash(): Partial<Options> | null {
  const h = window.location.hash.slice(1);
  if (!h) return null;
  const p = Object.fromEntries(new URLSearchParams(h)) as Record<string, string>;
  // #direction=rail starts from a curated combo; other keys override it.
  const dir = directions.find((d) => d.id === p.direction);
  return { ...(dir?.options ?? {}), ...p } as Partial<Options>;
}

function toHash(o: Options): string {
  const p = new URLSearchParams();
  for (const d of dimensions) if (o[d.id] !== defaults[d.id]) p.set(d.id, o[d.id]);
  return p.toString();
}

function load<T>(key: string, fallback: T): T {
  try {
    const v = localStorage.getItem(key);
    return v ? (JSON.parse(v) as T) : fallback;
  } catch {
    return fallback;
  }
}

function save(key: string, value: unknown) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* private window: the hash still keeps the options */
  }
}

type Store = {
  options: Options;
  set: (id: DimId, value: string) => void;
  apply: (o: Partial<Options>) => void;
  cycle: (id: DimId, step: number) => void;
  snapshots: Snapshot[];
  saveSnapshot: (name: string, note?: string) => void;
  removeSnapshot: (name: string) => void;
  ab: { a: Options | null; b: Options | null; showing: "a" | "b" | null };
  storeSlot: (slot: "a" | "b") => void;
  flipAB: () => void;
};

const Ctx = createContext<Store | null>(null);

export function OptionsProvider({ children }: { children: ReactNode }) {
  const [options, setOptions] = useState<Options>(() => valid({ ...load(KEY, {}), ...(fromHash() ?? {}) }));
  const [snapshots, setSnapshots] = useState<Snapshot[]>(() => load(SNAP_KEY, []));
  const [ab, setAB] = useState<Store["ab"]>(() => load(AB_KEY, { a: null, b: null, showing: null }));

  useEffect(() => {
    save(KEY, options);
    const keep = window.location.hash.match(/(lab|graph|page)=[^&]*/g) ?? [];
    const h = [toHash(options), ...keep].filter(Boolean).join("&");
    history.replaceState(null, "", h ? `#${h}` : window.location.pathname);
  }, [options]);
  useEffect(() => save(SNAP_KEY, snapshots), [snapshots]);
  useEffect(() => save(AB_KEY, ab), [ab]);
  useEffect(() => {
    const onHash = () => {
      const h = fromHash();
      if (h) setOptions(valid(h));
    };
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  const set = useCallback((id: DimId, value: string) => setOptions((o) => ({ ...o, [id]: value })), []);
  const apply = useCallback((p: Partial<Options>) => setOptions((o) => valid({ ...o, ...p })), []);
  const cycle = useCallback((id: DimId, step: number) => {
    setOptions((o) => {
      const d = dimensions.find((x) => x.id === id)!;
      const i = d.choices.findIndex((c) => c.id === o[id]);
      const n = d.choices.length;
      return { ...o, [id]: d.choices[(i + step + n) % n].id };
    });
  }, []);

  const value = useMemo<Store>(
    () => ({
      options, set, apply, cycle, snapshots, ab,
      saveSnapshot: (name, note) =>
        setSnapshots((s) => [...s.filter((x) => x.name !== name), { name, note, options }]),
      removeSnapshot: (name) => setSnapshots((s) => s.filter((x) => x.name !== name)),
      storeSlot: (slot) => setAB((s) => ({ ...s, [slot]: options, showing: slot })),
      flipAB: () =>
        setAB((s) => {
          const next = s.showing === "a" ? "b" : "a";
          const o = s[next];
          if (!o) return s;
          setOptions(o);
          return { ...s, showing: next };
        }),
    }),
    [options, set, apply, cycle, snapshots, ab],
  );
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useLab(): Store {
  const s = useContext(Ctx);
  if (!s) throw new Error("useLab outside OptionsProvider");
  return s;
}

export function useOpt<K extends DimId>(id: K): string {
  return useLab().options[id];
}
