// SPDX-License-Identifier: Apache-2.0
// Design tokens, named as the app's theme files name them (hosts/maya/themes/*.json "ui" keys),
// so a theme picked here exports straight to a theme JSON the Qt app already reads.
import type { Options } from "@/lab/options";

export type Tokens = {
  background: string; // window behind the docks, the viewport surround
  viewport: string; // the 3D clear color
  panel: string; // dock surface
  raised: string; // fields, buttons, cards
  hover: string; // hovered field, checked chip
  border: string; // 1px lines between and around things
  text: string;
  textDim: string; // labels
  textMuted: string; // hints, counts, shortcuts
  accent: string;
  accentText: string;
  selection: string;
  track: string; // slider track / unfilled field
  dark: boolean;
};

type Base = Omit<Tokens, "accent" | "accentText" | "selection">;

export const themes: Record<string, Base> = {
  charcoal: {
    background: "#141414", viewport: "#17181B", panel: "#1C1C1C", raised: "#262626", hover: "#303030", border: "#2A2A2A",
    text: "#EDEDED", textDim: "#A3A3A3", textMuted: "#737373", track: "#2E2E2E", dark: true,
  },
  graphite: {
    background: "#0F1114", viewport: "#14171B", panel: "#16191E", raised: "#1E2228", hover: "#282D35", border: "#23272E",
    text: "#E4E7EB", textDim: "#959CA6", textMuted: "#69707A", track: "#2A3038", dark: true,
  },
  carbon: {
    background: "#09090B", viewport: "#0E0E11", panel: "#111113", raised: "#1A1A1D", hover: "#242428", border: "#202024",
    text: "#FAFAFA", textDim: "#A1A1AA", textMuted: "#6B6B74", track: "#27272A", dark: true,
  },
  umber: {
    background: "#141311", viewport: "#191816", panel: "#1C1A18", raised: "#272421", hover: "#322E2A", border: "#2B2825",
    text: "#EDE9E3", textDim: "#A69F95", textMuted: "#776F66", track: "#35312C", dark: true,
  },
  paper: {
    background: "#E9E9EC", viewport: "#D9DADE", panel: "#F7F7F8", raised: "#FFFFFF", hover: "#ECECEF", border: "#DEDEE2",
    text: "#18181B", textDim: "#5F5F68", textMuted: "#8E8E97", track: "#DCDCE0", dark: false,
  },
};

export const accents: Record<string, { dark: [string, string]; light: [string, string] }> = {
  // [accent, text on accent] for dark and light themes
  mono: { dark: ["#F2F2F2", "#0A0A0A"], light: ["#18181B", "#FAFAFA"] },
  blue: { dark: ["#5B8DEF", "#FFFFFF"], light: ["#2F6FE4", "#FFFFFF"] },
  emerald: { dark: ["#3FB68B", "#04140D"], light: ["#14946A", "#FFFFFF"] },
  amber: { dark: ["#E3AE45", "#1A1205"], light: ["#B7801A", "#FFFFFF"] },
  violet: { dark: ["#8E7CF6", "#FFFFFF"], light: ["#6B57E8", "#FFFFFF"] },
  coral: { dark: ["#EB7B62", "#1A0905"], light: ["#D35A3F", "#FFFFFF"] },
};

function mix(a: string, b: string, t: number): string {
  const pa = [1, 3, 5].map((i) => parseInt(a.slice(i, i + 2), 16));
  const pb = [1, 3, 5].map((i) => parseInt(b.slice(i, i + 2), 16));
  return "#" + pa.map((v, i) => Math.round(v + (pb[i] - v) * t).toString(16).padStart(2, "0")).join("").toUpperCase();
}

export function tokensFor(o: Options): Tokens {
  const base = themes[o.theme] ?? themes.charcoal;
  const [accent, accentText] = (accents[o.accent] ?? accents.mono)[base.dark ? "dark" : "light"];
  // Mono's selection is a lighter raised tone; colored accents tint it.
  const selection = o.accent === "mono" ? base.hover : mix(base.raised, accent, base.dark ? 0.28 : 0.18);
  return { ...base, accent, accentText, selection };
}

export const density = {
  compact: { row: 22, font: 11, small: 10, pad: 8, gap: 4 },
  default: { row: 26, font: 12, small: 11, pad: 10, gap: 6 },
  comfy: { row: 30, font: 13, small: 11, pad: 12, gap: 8 },
} as const;

export const fonts: Record<string, string> = {
  figtree: "Figtree, system-ui, sans-serif",
  inter: "Inter, system-ui, sans-serif",
  geist: "Geist, system-ui, sans-serif",
  system: "'Segoe UI', system-ui, sans-serif",
};

// The CSS variables every component reads (index.css maps them to Tailwind colors).
export function cssVars(o: Options): Record<string, string> {
  const t = tokensFor(o);
  const d = density[o.density as keyof typeof density] ?? density.default;
  const r = Number(o.radius);
  return {
    "--gl-background": t.background,
    "--gl-viewport": t.viewport,
    "--gl-panel": t.panel,
    "--gl-raised": t.raised,
    "--gl-hover": t.hover,
    "--gl-border": t.border,
    "--gl-text": t.text,
    "--gl-dim": t.textDim,
    "--gl-muted": t.textMuted,
    "--gl-accent": t.accent,
    "--gl-on-accent": t.accentText,
    "--gl-selection": t.selection,
    "--gl-track": t.track,
    "--gl-row": `${d.row}px`,
    "--gl-font": `${d.font}px`,
    "--gl-small": `${d.small}px`,
    "--gl-pad": `${d.pad}px`,
    "--gl-gap": `${d.gap}px`,
    "--gl-radius": `${r}px`,
    "--gl-radius-lg": `${r === 0 ? 0 : r + 2}px`,
    "--gl-sans": fonts[o.font] ?? fonts.figtree,
    "--gl-num": o.numbers === "mono" ? "'JetBrains Mono', ui-monospace, monospace" : "var(--gl-sans)",
  };
}
