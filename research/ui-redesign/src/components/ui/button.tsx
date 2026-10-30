// SPDX-License-Identifier: Apache-2.0
// Qt: QPushButton / QToolButton. Variants map to a dynamic property: QPushButton[role="primary"].
import { cva, type VariantProps } from "class-variance-authority";
import type { ButtonHTMLAttributes } from "react";
import { cn } from "@/lib/utils";

export const buttonVariants = cva(
  "inline-flex items-center justify-center gap-1.5 whitespace-nowrap rounded-ctl font-medium text-ui outline-none select-none " +
    "focus-visible:border-accent disabled:pointer-events-none disabled:opacity-40 border",
  {
    variants: {
      variant: {
        default: "bg-raised border-line text-fg hover:bg-hover active:bg-selection",
        primary: "bg-accent border-accent text-on-accent hover:opacity-90 font-semibold",
        ghost: "bg-transparent border-transparent text-dim hover:bg-hover hover:text-fg",
        outline: "bg-transparent border-line text-fg hover:bg-hover",
        checked: "bg-selection border-transparent text-fg",
      },
      size: {
        sm: "h-[calc(var(--gl-row)-4px)] px-2 text-sm-ui",
        md: "h-row px-3",
        icon: "size-row p-0",
        iconSm: "size-[calc(var(--gl-row)-4px)] p-0",
      },
    },
    defaultVariants: { variant: "default", size: "md" },
  },
);

export type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> & VariantProps<typeof buttonVariants>;

export function Button({ className, variant, size, ...props }: ButtonProps) {
  return <button className={cn(buttonVariants({ variant, size }), className)} {...props} />;
}
