// SPDX-License-Identifier: Apache-2.0
// Qt: QTabBar. Each style is one QSS block on QTabBar::tab / ::tab:selected / ::tab:hover.
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";

export type TabItem = { id: string; label: string };

export function TabBar({ items, value, onChange, className, stretch = true }: {
  items: TabItem[]; value: string; onChange: (id: string) => void; className?: string; stretch?: boolean;
}) {
  const style = useOpt("tabs");
  return (
    <div
      role="tablist"
      className={cn(
        "flex items-center",
        style === "segmented" && "gap-0.5 rounded-ctl bg-background p-0.5 border border-line",
        style === "underline" && "gap-3 border-b border-line",
        style === "pills" && "gap-1",
        className,
      )}
    >
      {items.map((t) => {
        const on = t.id === value;
        return (
          <button
            key={t.id}
            role="tab"
            aria-selected={on}
            onClick={() => onChange(t.id)}
            className={cn(
              "outline-none font-medium whitespace-nowrap",
              stretch && style !== "underline" && "flex-1",
              style === "segmented" && [
                "h-[calc(var(--gl-row)-6px)] rounded-[calc(var(--gl-radius)-2px)] px-2 text-sm-ui",
                on ? "bg-raised text-fg" : "text-dim hover:text-fg",
              ],
              style === "underline" && [
                "-mb-px h-row border-b-2 text-ui",
                on ? "border-accent text-fg" : "border-transparent text-dim hover:text-fg",
              ],
              style === "pills" && [
                "h-[calc(var(--gl-row)-4px)] rounded-full px-3 text-sm-ui",
                on ? "bg-accent text-on-accent" : "text-dim hover:bg-hover hover:text-fg",
              ],
            )}
          >
            {t.label}
          </button>
        );
      })}
    </div>
  );
}
