// SPDX-License-Identifier: Apache-2.0
// The Maya-first mock's state: the Groom Tools page and tool, what the brushes edit (guide group,
// layer), Mirror, the open windows, a message line.
import { createContext, useCallback, useContext, useRef, useState, type ReactNode } from "react";
import { useWS } from "@/workspace/state";
import { correctiveTools } from "./data";

export type Page = "tools" | "paint" | "photos";
export type Toast = { id: number; text: string };

type S = {
  page: Page; setPage: (p: Page) => void;
  tool: string; setTool: (t: string) => void;
  /** What brushes edit: the picked guide group, and the base groom or a corrective layer. */
  group: string; setGroup: (g: string) => void;
  layer: string; setLayer: (l: string) => void;
  /** Mirror: one setting for every brush (Alt+Shift+X), and across what. */
  mirror: boolean; setMirror: (v: boolean) => void;
  mirrorAxis: string; setMirrorAxis: (v: string) => void;
  map: string; setMap: (m: string) => void;
  /** The Tube brush's job, and in Roots the root mode. */
  tubeMode: "sculpt" | "roots" | "shape"; setTubeMode: (m: "sculpt" | "roots" | "shape") => void;
  rootMode: "draw" | "edit" | "select"; setRootMode: (m: "draw" | "edit" | "select") => void;
  hasGroom: boolean; setHasGroom: (v: boolean) => void;
  meshSelected: boolean; setMeshSelected: (v: boolean) => void;
  search: boolean; setSearch: (v: boolean) => void;
  graph: boolean; setGraph: (v: boolean) => void; graphMax: boolean;
  toast: Toast | null; say: (text: string) => void;
};

export const BASE = "Base groom";
const Ctx = createContext<S | null>(null);
const hash = (k: string) => window.location.hash.match(new RegExp(`[#&]${k}=([\\w-]+)`))?.[1];

export function SimpleProvider({ children }: { children: ReactNode }) {
  const [page, setPageRaw] = useState<Page>(() => (hash("tab") as Page) ?? "tools");
  const [tool, setToolRaw] = useState(() => hash("tool") ?? (hash("tab") === "paint" ? "paint" : "comb"));
  const [group, setGroup] = useState("primary");
  const [layer, setLayerRaw] = useState(() => (hash("layer") ? "wind_pose" : BASE));
  const [mirror, setMirror] = useState(false);
  const [mirrorAxis, setMirrorAxis] = useState("World X");
  const [map, setMap] = useState("density");
  const [tubeMode, setTubeMode] = useState<"sculpt" | "roots" | "shape">(() => (hash("tube") as "sculpt" | "roots" | "shape") ?? "sculpt");
  const [rootMode, setRootMode] = useState<"draw" | "edit" | "select">("draw");
  const [hasGroom, setHasGroom] = useState(() => hash("empty") !== "1");
  const [meshSelected, setMeshSelected] = useState(false);
  const [search, setSearch] = useState(() => hash("search") === "1");
  const [toast, setToast] = useState<Toast | null>(null);
  // The Graph Editor is the shared mock (src/workspace/graph): its own close / maximize drive this.
  const ws = useWS();
  const timer = useRef<number | undefined>(undefined);
  const say = useCallback((text: string) => {
    window.clearTimeout(timer.current);
    setToast({ id: Date.now(), text });
    timer.current = window.setTimeout(() => setToast(null), 3500);
  }, []);
  // The Paint tab is painting: opening it starts Paint on the picked map; leaving it gives back the
  // brush you had before.
  const before = useRef("comb");
  const setPage = (p: Page) => {
    setPageRaw(p);
    if (p === "paint") { if (tool !== "paint") before.current = tool; setToolRaw("paint"); }
    else if (tool === "paint") setToolRaw(before.current);
  };
  const setTool = (t: string) => {
    if (layer !== BASE && !correctiveTools.includes(t)) return; // greyed out on a layer
    setToolRaw(t);
  };
  // On a corrective layer only the brushes that edit layers work: a brush that can't falls back to Comb.
  const setLayer = (l: string) => {
    setLayerRaw(l);
    if (l !== BASE && !correctiveTools.includes(tool) && tool !== "paint") setToolRaw("comb");
  };
  return (
    <Ctx.Provider
      value={{
        page, setPage, tool, setTool, group, setGroup, layer, setLayer, mirror, setMirror, mirrorAxis, setMirrorAxis, map, setMap, tubeMode, setTubeMode, rootMode, setRootMode,
        hasGroom, setHasGroom, meshSelected, setMeshSelected, search, setSearch,
        graph: ws.graphOpen, setGraph: ws.setGraphOpen, graphMax: ws.graphMax, toast, say,
      }}
    >
      {children}
    </Ctx.Provider>
  );
}

export function useS(): S {
  const c = useContext(Ctx);
  if (!c) throw new Error("useS outside SimpleProvider");
  return c;
}
