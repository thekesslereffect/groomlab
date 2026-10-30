// SPDX-License-Identifier: Apache-2.0
// Qt: QCheckBox (::indicator:checked with a check image) or a small custom-painted switch
// (a QAbstractButton subclass painting a rounded rect + knob; ~40 lines).
import { Check } from "lucide-react";
import { useOpt } from "@/lab/store";
import { cn } from "@/lib/utils";

export function Toggle({ checked, onChange, label }: { checked: boolean; onChange?: (v: boolean) => void; label?: string }) {
  const style = useOpt("booleans");
  const box =
    style === "switch" ? (
      <span
        className={cn(
          "relative inline-block h-4 w-7 shrink-0 rounded-full border",
          checked ? "border-accent bg-accent" : "border-line bg-track",
        )}
      >
        <span
          className={cn(
            "absolute top-1/2 size-3 -translate-y-1/2 rounded-full",
            checked ? "left-[13px] bg-on-accent" : "left-[1px] bg-dim",
          )}
        />
      </span>
    ) : (
      <span
        className={cn(
          "inline-flex size-4 shrink-0 items-center justify-center rounded-[min(var(--gl-radius),4px)] border",
          checked ? "border-accent bg-accent text-on-accent" : "border-line bg-raised",
        )}
      >
        {checked && <Check className="size-3" strokeWidth={3} />}
      </span>
    );
  return (
    <button
      role="checkbox"
      aria-checked={checked}
      onClick={() => onChange?.(!checked)}
      className="inline-flex h-row items-center gap-2 text-ui text-fg outline-none"
    >
      {box}
      {label && <span>{label}</span>}
    </button>
  );
}
