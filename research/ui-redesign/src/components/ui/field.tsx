// SPDX-License-Identifier: Apache-2.0
// Property rows and value controls.
// Qt: Row = a QFormLayout row (split) or a QVBoxLayout (stacked).
//     FillField = one custom-painted QWidget (paintEvent: rounded rect, fill rect, two texts;
//     drag scrubs, a click without a drag opens an inline QLineEdit over it). The app's param_editors
//     already paint their own sliders, so this is a restyle, not new machinery.
//     TrackSlider = QSlider (groove / sub-page / handle in QSS) + QLineEdit.
import { Slider as RSlider } from "radix-ui";
import { useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";

export function Row({ label, children, hint, className }: { label?: string; children: ReactNode; hint?: string; className?: string }) {
  const rows = useOpt("rows");
  if (rows === "stacked" && label) {
    return (
      <div className={cn("flex flex-col gap-1 py-0.5", className)} title={hint}>
        <span className="text-sm-ui text-dim">{label}</span>
        <div className="flex min-w-0 items-center gap-gap">{children}</div>
      </div>
    );
  }
  return (
    <div className={cn("grid min-h-row grid-cols-[38%_1fr] items-center gap-gap", className)} title={hint}>
      <span className="truncate text-dim">{label}</span>
      <div className="flex min-w-0 items-center gap-gap">{children}</div>
    </div>
  );
}

function fmt(v: number, step: number) {
  const d = step >= 1 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
  return v.toFixed(d);
}

/** A typed value: a number or simple arithmetic ("0.5*2", "1/3"), as Maya and Blender fields take. */
export function parseValue(text: string): number | null {
  const s = text.trim().replace(/,/g, ".");
  if (!s || !/^[\d.+\-*/() eE]+$/.test(s)) return null;
  try {
    const v = Number(Function(`"use strict"; return (${s});`)());
    return Number.isFinite(v) ? v : null;
  } catch {
    return null;
  }
}

/** A text box for a number: keeps what is typed until Enter / Tab / leaving it, Esc puts the value back.
 *  Qt: QLineEdit with a QDoubleValidator-free parse on editingFinished, Esc via keyPressEvent. */
function NumberInput({ value, step, onCommit, onDone, autoFocus, className }: {
  value: number; step: number; onCommit: (v: number) => void; onDone?: () => void; autoFocus?: boolean; className?: string;
}) {
  const [draft, setDraft] = useState<string | null>(autoFocus ? fmt(value, step) : null);
  const cancelled = useRef(false);
  const input = useRef<HTMLInputElement>(null);
  // Opened by a click on a fill field: focus with the text selected, so typing replaces it.
  useLayoutEffect(() => {
    if (autoFocus) {
      input.current?.focus();
      input.current?.select();
    }
  }, [autoFocus]);
  const commit = () => {
    if (draft !== null && !cancelled.current) {
      const v = parseValue(draft);
      if (v !== null) onCommit(v);
    }
    cancelled.current = false;
    setDraft(null);
    onDone?.();
  };
  return (
    <input
      ref={input}
      onFocus={(e) => {
        if (draft !== null) return;
        setDraft(fmt(value, step));
        const el = e.target;
        setTimeout(() => el.select());
      }}
      value={draft ?? fmt(value, step)}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === "Enter") (e.target as HTMLInputElement).blur();
        if (e.key === "Escape") {
          cancelled.current = true;
          (e.target as HTMLInputElement).blur();
        }
      }}
      className={cn("num select-text bg-transparent text-fg outline-none", className)}
    />
  );
}

/** A slider drawn as a field (Blender-like): drag to scrub (Shift: finer, Ctrl: snap to steps of 10),
 *  click to type a value (typed values may go past the slider's range), arrows nudge when focused. */
export function FillField({ label, value, min = 0, max = 1, step = 0.01, onChange, suffix, className }: {
  label?: string; value: number; min?: number; max?: number; step?: number; onChange: (v: number) => void; suffix?: string; className?: string;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const drag = useRef<{ x: number; v: number; moved: boolean } | null>(null);
  const [editing, setEditing] = useState(false);
  const t = Math.max(0, Math.min(1, (value - min) / (max - min)));
  const snap = (v: number, s: number) => Math.round(v / s) * s;
  const clamp = (v: number) => Math.max(min, Math.min(max, v));

  return (
    <div
      ref={ref}
      tabIndex={editing ? -1 : 0}
      onPointerDown={(e) => {
        if (editing || e.button !== 0) return;
        (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
        drag.current = { x: e.clientX, v: value, moved: false };
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d) return;
        const dx = e.clientX - d.x;
        if (!d.moved && Math.abs(dx) < 3) return;
        d.moved = true;
        const width = ref.current!.getBoundingClientRect().width;
        const v = d.v + (dx / width) * (max - min) * (e.shiftKey ? 0.1 : 1);
        onChange(clamp(snap(v, e.ctrlKey ? step * 10 : step)));
      }}
      onPointerUp={() => {
        const d = drag.current;
        drag.current = null;
        if (d && !d.moved) setEditing(true);
      }}
      onKeyDown={(e) => {
        if (editing) return;
        if (e.key === "Enter" || e.key === "F2") setEditing(true);
        if (e.key === "ArrowRight" || e.key === "ArrowUp") onChange(clamp(snap(value + step * (e.shiftKey ? 10 : 1), step)));
        if (e.key === "ArrowLeft" || e.key === "ArrowDown") onChange(clamp(snap(value - step * (e.shiftKey ? 10 : 1), step)));
      }}
      className={cn(
        "relative flex h-row w-full items-center overflow-hidden rounded-ctl border px-2 text-ui outline-none",
        editing ? "cursor-text border-accent bg-background" : "cursor-ew-resize border-line bg-raised hover:bg-hover focus-visible:border-accent",
        className,
      )}
    >
      {!editing && <div className="absolute inset-y-0 left-0 bg-selection" style={{ width: `${t * 100}%` }} />}
      {label && <span className="relative truncate text-dim">{label}</span>}
      {editing ? (
        <NumberInput
          autoFocus
          value={value}
          step={step}
          onCommit={onChange}
          onDone={() => {
            setEditing(false);
            ref.current?.focus();
          }}
          className={cn("relative min-w-0 flex-1", label ? "pl-2 text-right" : "text-center")}
        />
      ) : (
        <span className={cn("num relative text-fg", label ? "ml-auto pl-2" : "mx-auto")}>
          {fmt(value, step)}
          {suffix}
        </span>
      )}
    </div>
  );
}

export function TrackSlider({ value, min = 0, max = 1, step = 0.01, onChange }: {
  value: number; min?: number; max?: number; step?: number; onChange: (v: number) => void;
}) {
  return (
    <div className="flex w-full items-center gap-gap">
      <RSlider.Root className="relative flex h-row flex-1 touch-none items-center" value={[Math.max(min, Math.min(max, value))]} min={min} max={max} step={step} onValueChange={(v) => onChange(v[0])}>
        <RSlider.Track className="relative h-1 grow rounded-full bg-track">
          <RSlider.Range className="absolute h-full rounded-full bg-accent" />
        </RSlider.Track>
        <RSlider.Thumb className="block size-3 rounded-full border border-line bg-fg outline-none focus-visible:border-accent" />
      </RSlider.Root>
      <NumberInput
        value={value}
        step={step}
        onCommit={onChange}
        className="h-row w-14 rounded-ctl border border-line bg-raised px-1.5 text-right text-ui focus:border-accent"
      />
    </div>
  );
}

/** A numeric property: picks the row layout and slider style from the options. */
export function Param({ label, init, min = 0, max = 1, step = 0.01, suffix, hint }: {
  label: string; init: number; min?: number; max?: number; step?: number; suffix?: string; hint?: string;
}) {
  const [v, setV] = useState(init);
  const rows = useOpt("rows");
  const sliders = useOpt("sliders");
  if (rows === "fields") {
    return sliders === "fill" ? (
      <div className="py-px" title={hint}>
        <FillField label={label} value={v} min={min} max={max} step={step} onChange={setV} suffix={suffix} />
      </div>
    ) : (
      <Row label={label} hint={hint}><TrackSlider value={v} min={min} max={max} step={step} onChange={setV} /></Row>
    );
  }
  return (
    <Row label={label} hint={hint}>
      {sliders === "fill" ? (
        <FillField value={v} min={min} max={max} step={step} onChange={setV} suffix={suffix} />
      ) : (
        <TrackSlider value={v} min={min} max={max} step={step} onChange={setV} />
      )}
    </Row>
  );
}

export function TextValue({ children, className }: { children: ReactNode; className?: string }) {
  return <span className={cn("num truncate text-fg", className)}>{children}</span>;
}

export function Swatch({ color, value }: { color: string; value: string }) {
  return (
    <div className="flex w-full items-center gap-gap">
      <span className="h-row w-8 shrink-0 rounded-ctl border border-line" style={{ background: color }} />
      <span className="num flex h-row flex-1 items-center rounded-ctl border border-line bg-raised px-2 text-fg">{value}</span>
    </div>
  );
}

export function Kbd({ children }: { children: ReactNode }) {
  return <span className="num rounded-[3px] border border-line px-1 text-[10px] leading-4 text-muted">{children}</span>;
}
