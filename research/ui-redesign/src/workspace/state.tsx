// SPDX-License-Identifier: Apache-2.0
import { createContext, useContext, useState, type ReactNode } from "react";

type WS = {
  tool: string; setTool: (t: string) => void;
  graphOpen: boolean; setGraphOpen: (v: boolean) => void;
  graphMax: boolean; setGraphMax: (v: boolean) => void;
  toolsTab: string; setToolsTab: (t: string) => void;
  propsTab: string; setPropsTab: (t: string) => void;
  railPanel: "tools" | "properties"; setRailPanel: (p: "tools" | "properties") => void;
};

const Ctx = createContext<WS | null>(null);

export function WorkspaceProvider({ children }: { children: ReactNode }) {
  const [tool, setTool] = useState(() => window.location.hash.match(/tool=(\w+)/)?.[1] ?? "comb");
  const [graphOpen, setGraphOpen] = useState(() => /(^|[#&])graph=[12]/.test(window.location.hash));
  const [graphMax, setGraphMax] = useState(() => /(^|[#&])graph=2/.test(window.location.hash));
  const [toolsTab, setToolsTab] = useState(() => window.location.hash.match(/tab=(\w+)/)?.[1] ?? "tools");
  const [propsTab, setPropsTab] = useState("attributes");
  const [railPanel, setRailPanel] = useState<"tools" | "properties">("properties");
  return (
    <Ctx.Provider value={{ tool, setTool, graphOpen, setGraphOpen, graphMax, setGraphMax, toolsTab, setToolsTab, propsTab, setPropsTab, railPanel, setRailPanel }}>
      {children}
    </Ctx.Provider>
  );
}

export function useWS(): WS {
  const c = useContext(Ctx);
  if (!c) throw new Error("useWS outside WorkspaceProvider");
  return c;
}
