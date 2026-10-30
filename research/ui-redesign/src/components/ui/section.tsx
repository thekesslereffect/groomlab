// SPDX-License-Identifier: Apache-2.0
// Qt: a header QToolButton (checkable, arrow icon, text) toggling a body QWidget's visibility.
// Cards = the section wrapped in a QFrame[role="card"] (background + border + radius in QSS).
import { ChevronRight } from "lucide-react";
import { useState, type ReactNode } from "react";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";

export function Section({ title, children, open: initial = true, actions, count }: {
  title: string; children: ReactNode; open?: boolean; actions?: ReactNode; count?: ReactNode;
}) {
  const [open, setOpen] = useState(initial);
  const style = useOpt("sections");
  return (
    <section
      className={cn(
        style === "cards" && "rounded-pnl border border-line bg-raised/40 px-pad pb-1",
        style === "caps" && "border-t border-line first:border-t-0",
      )}
    >
      <div className="flex h-row items-center gap-1">
        <button onClick={() => setOpen(!open)} className="group flex h-full min-w-0 flex-1 items-center gap-1.5 text-left outline-none">
          <ChevronRight className={cn("size-3 shrink-0 text-muted", open && "rotate-90")} />
          <span
            className={cn(
              "truncate",
              style === "caps" && "text-sm-ui font-semibold uppercase tracking-[0.08em] text-muted group-hover:text-dim",
              style === "cards" && "font-semibold text-fg",
              style === "plain" && "font-semibold text-fg",
            )}
          >
            {title}
          </span>
          {count != null && <span className="num text-sm-ui text-muted">{count}</span>}
        </button>
        {actions}
      </div>
      {open && <div className={cn("flex flex-col gap-0.5 pb-2", style !== "cards" && "pl-0")}>{children}</div>}
    </section>
  );
}
