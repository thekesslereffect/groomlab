// SPDX-License-Identifier: Apache-2.0
// Qt: QToolTip (QSS QToolTip { ... }); the shortcut is a second, dimmer span (rich text).
import { Tooltip as T } from "radix-ui";
import type { ReactNode } from "react";

export function Tip({ label, shortcut, children, side = "bottom" }: { label: ReactNode; shortcut?: string; children: ReactNode; side?: "top" | "bottom" | "left" | "right" }) {
  return (
    <T.Root delayDuration={350}>
      <T.Trigger asChild>{children}</T.Trigger>
      <T.Portal>
        <T.Content
          side={side}
          sideOffset={6}
          className="gl-ui z-50 rounded-ctl border border-line bg-raised px-2 py-1 text-sm-ui text-fg"
        >
          {label}
          {shortcut && <span className="ml-2 text-muted">{shortcut}</span>}
        </T.Content>
      </T.Portal>
    </T.Root>
  );
}

export const TipProvider = T.Provider;
