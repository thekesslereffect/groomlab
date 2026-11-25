// SPDX-License-Identifier: Apache-2.0
// Small parts the Simple workspace is built from. Each maps to a stock Qt widget or a small
// custom-painted one (noted per part), so the same panel can be drawn in Maya (PySide) and
// Blender (uikit) too.
import { ChevronRight } from "lucide-react";
import { useState, type ReactNode } from "react";
import { cn } from "@/lib/utils";

/** One-of choice. Qt: QButtonGroup of checkable QToolButtons in a QFrame. */
export function Seg<T extends string>({ items, value, onChange, className, size = "md" }: {
  items: { id: T; label: ReactNode; tip?: string }[]; value: T; onChange: (v: T) => void; className?: string; size?: "sm" | "md";
}) {
  return (
    <div className={cn("flex rounded-ctl border border-line bg-raised p-0.5", className)}>
      {items.map((it) => (
        <button
          key={it.id}
          title={it.tip}
          onClick={() => onChange(it.id)}
          className={cn(
            "flex flex-1 items-center justify-center gap-1 whitespace-nowrap rounded-[calc(var(--gl-radius)-2px)] px-1.5 outline-none",
            size === "sm" ? "h-[calc(var(--gl-row)-8px)] text-sm-ui" : "h-[calc(var(--gl-row)-4px)]",
            value === it.id ? "bg-selection font-medium text-fg" : "text-dim hover:text-fg",
          )}
        >
          {it.label}
        </button>
      ))}
    </div>
  );
}

/** "More …": hides what most people never need, says how much is behind it.
 *  Qt: a flat checkable QToolButton toggling a QWidget. */
export function More({ label, count, children, open: initial = false }: { label: string; count?: number; children: ReactNode; open?: boolean }) {
  const [open, setOpen] = useState(initial);
  return (
    <div className="flex flex-col">
      <button onClick={() => setOpen(!open)} className="flex h-row items-center gap-1 text-left text-sm-ui text-muted outline-none hover:text-fg">
        <ChevronRight className={cn("size-3", open && "rotate-90")} />
        {label}
        {count != null && !open && <span className="num ml-auto rounded-full bg-raised px-1.5 text-[10px] leading-4">{count}</span>}
      </button>
      {open && <div className="flex flex-col gap-gap pt-1">{children}</div>}
    </div>
  );
}

/** A titled block of the panel. Qt: a QLabel header over a QVBoxLayout. */
export function Block({ title, aside, children, className }: { title?: ReactNode; aside?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={cn("flex flex-col gap-gap", className)}>
      {(title || aside) && (
        <div className="flex h-5 items-center gap-2">
          {title && <h3 className="text-sm-ui font-semibold text-dim">{title}</h3>}
          {aside && <div className="ml-auto flex items-center gap-1">{aside}</div>}
        </div>
      )}
      {children}
    </section>
  );
}

/** On/off chip. Qt: checkable QToolButton with an icon and text. */
export function Chip({ on, onClick, children, tip }: { on: boolean; onClick: () => void; children: ReactNode; tip?: string }) {
  return (
    <button
      title={tip}
      onClick={onClick}
      className={cn(
        "flex h-[calc(var(--gl-row)-2px)] items-center gap-1.5 rounded-full border px-2.5 text-sm-ui outline-none",
        on ? "border-accent/60 bg-selection text-fg" : "border-line text-dim hover:bg-hover hover:text-fg",
      )}
    >
      {children}
    </button>
  );
}

export function Key({ children }: { children: ReactNode }) {
  return <span className="num inline-flex h-4 min-w-4 items-center justify-center rounded-[3px] border border-line bg-background/60 px-1 text-[10px] leading-none text-dim">{children}</span>;
}
