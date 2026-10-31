// SPDX-License-Identifier: Apache-2.0
import { clsx, type ClassValue } from "clsx";
import { extendTailwindMerge } from "tailwind-merge";

// text-ui / text-sm-ui are font sizes (index.css), not colors: tell tailwind-merge so it keeps
// them next to text-fg / text-dim.
const twMerge = extendTailwindMerge({
  extend: { classGroups: { "font-size": [{ text: ["ui", "sm-ui"] }] } },
});

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}
