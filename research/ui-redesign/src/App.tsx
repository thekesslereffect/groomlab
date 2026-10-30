// SPDX-License-Identifier: Apache-2.0
import { useLayoutEffect, useState } from "react";
import { Export } from "@/gallery/Export";
import { Gallery } from "@/gallery/Gallery";
import { LabPanel, type Page } from "@/lab/LabPanel";
import { useLab } from "@/lab/store";
import { TipProvider } from "@/components/ui/tooltip";
import { cssVars } from "@/theme/tokens";
import { WorkspaceProvider } from "@/workspace/state";
import { Shell } from "@/workspace/shell";

export function App() {
  const { options } = useLab();
  const [page, setPage] = useState<Page>(() => (window.location.hash.match(/page=(\w+)/)?.[1] as Page) ?? "workspace");

  // Tokens on <html> so portaled menus and tooltips get them too; data-* for CSS that wants them.
  useLayoutEffect(() => {
    const root = document.documentElement;
    for (const [k, v] of Object.entries(cssVars(options))) root.style.setProperty(k, v);
    for (const [k, v] of Object.entries(options)) root.dataset[k] = v;
  }, [options]);

  return (
    <TipProvider>
      <WorkspaceProvider>
        <div className="flex h-full">
          <div className="min-w-0 flex-1">
            {page === "workspace" ? <Shell /> : page === "components" ? <Gallery /> : <Export />}
          </div>
          <LabPanel page={page} setPage={setPage} />
        </div>
      </WorkspaceProvider>
    </TipProvider>
  );
}
