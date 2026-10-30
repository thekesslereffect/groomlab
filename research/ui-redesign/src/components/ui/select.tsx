// SPDX-License-Identifier: Apache-2.0
// Qt: QComboBox (drop-down arrow = ::down-arrow image, the list = QAbstractItemView in QSS).
import { Select as S } from "radix-ui";
import { Check, ChevronDown } from "lucide-react";
import { cn } from "@/lib/utils";

export function Select({ value, options, onChange, className }: {
  value: string; options: string[]; onChange?: (v: string) => void; className?: string;
}) {
  return (
    <S.Root value={value} onValueChange={onChange}>
      <S.Trigger
        className={cn(
          "inline-flex h-row min-w-0 items-center justify-between gap-1 rounded-ctl border border-line bg-raised px-2 text-ui text-fg outline-none",
          "hover:bg-hover focus-visible:border-accent data-[state=open]:border-accent",
          className,
        )}
      >
        <span className="truncate"><S.Value /></span>
        <ChevronDown className="size-3.5 shrink-0 text-muted" />
      </S.Trigger>
      <S.Portal>
        <S.Content position="popper" sideOffset={4} className="gl-ui z-50 min-w-[var(--radix-select-trigger-width)] rounded-ctl border border-line bg-raised p-1">
          <S.Viewport>
            {options.map((o) => (
              <S.Item
                key={o}
                value={o}
                className="relative flex h-row cursor-default items-center rounded-[calc(var(--gl-radius)-2px)] pl-6 pr-2 text-ui text-fg outline-none data-[highlighted]:bg-selection"
              >
                <S.ItemIndicator className="absolute left-1.5"><Check className="size-3.5" /></S.ItemIndicator>
                <S.ItemText>{o}</S.ItemText>
              </S.Item>
            ))}
          </S.Viewport>
        </S.Content>
      </S.Portal>
    </S.Root>
  );
}
