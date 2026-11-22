// SPDX-License-Identifier: Apache-2.0
/*
 * The Graph Editor's categories in the Playful theme's colors (engine/assets/themes/playful.json, filled
 * headers), for the dark pages: `hue` fills a node's header, `on` is the text on it, `tint` a
 * translucent fill of the hue over the dark, `ink` a lighter cut for lines and text on the dark
 * (AA on #0b0c0f and on the panels).
 */

export type Category =
  | 'Sources'
  | 'Grow'
  | 'Shape'
  | 'Look'
  | 'Color'
  | 'Fields'
  | 'Utility'
  | 'Motion'
  | 'Feathers'
  | 'Convert'
  | 'Games'
  | 'Output';

export const category: Record<Category, { hue: string; on: string; tint: string; ink: string }> = {
  Sources: { hue: '#2ECC8F', on: '#0b3d2a', tint: '#2ECC8F40', ink: '#5fe0ae' },
  Grow: { hue: '#4FD1C5', on: '#0b3a36', tint: '#4FD1C540', ink: '#78e0d6' },
  Shape: { hue: '#2F7BF6', on: '#ffffff', tint: '#2F7BF64d', ink: '#7cb0ff' },
  Look: { hue: '#FFC53D', on: '#3d2c00', tint: '#FFC53D40', ink: '#ffd36e' },
  Color: { hue: '#FF7AB6', on: '#4a0a28', tint: '#FF7AB640', ink: '#ff9fcb' },
  Fields: { hue: '#A36BFF', on: '#ffffff', tint: '#A36BFF4d', ink: '#c09bff' },
  Utility: { hue: '#8A93A0', on: '#16191d', tint: '#8A93A040', ink: '#b3bac4' },
  Motion: { hue: '#78C8FF', on: '#0a3352', tint: '#78C8FF40', ink: '#9ad6ff' },
  Feathers: { hue: '#FFC857', on: '#3d2c00', tint: '#FFC85740', ink: '#ffd780' },
  Convert: { hue: '#FF8A5C', on: '#4a1a06', tint: '#FF8A5C40', ink: '#ffa886' },
  Games: { hue: '#5EC8FF', on: '#06324d', tint: '#5EC8FF40', ink: '#8ad8ff' },
  Output: { hue: '#ECE7E1', on: '#141518', tint: '#ECE7E133', ink: '#ECE7E1' },
};

/** The dark node card: its body, hairline, labels, values and slider track (svg-graph, try-graph). */
export const surface = {
  body: '#14161b',
  line: '#2a2d35',
  label: '#9ea3ad',
  value: '#ecebe8',
  track: '#262930',
  /** The page behind the nodes: ports are ringed in it. */
  paper: '#0b0c0f',
} as const;

/** Wire and port colors by data type (Playful's ports). */
export const port = {
  strands: '#FFC53D',
  guides: '#2F7BF6',
  mesh: '#2ECC8F',
  field: '#A36BFF',
  value: '#7CC8B4',
} as const;

export type PortType = keyof typeof port;

/** The mark's three strands. */
export const brand = { red: '#FF4D4F', yellow: '#FFC53D', blue: '#2F7BF6' } as const;

/** A cubic between two ports; `from`/`to` say which side of its node each port is on. */
export type Side = 'l' | 'r' | 't' | 'b';
const dir: Record<Side, [number, number]> = { l: [-1, 0], r: [1, 0], t: [0, -1], b: [0, 1] };

export function wirePath(x1: number, y1: number, x2: number, y2: number, from: Side = 'r', to: Side = 'l', pull?: number) {
  const horizontal = from === 'l' || from === 'r';
  const k = pull ?? Math.max(36, Math.abs(horizontal ? x2 - x1 : y2 - y1) * 0.5);
  const [ax, ay] = dir[from];
  const [bx, by] = dir[to];
  const r = (n: number) => Math.round(n * 10) / 10;
  return `M${r(x1)} ${r(y1)} C${r(x1 + ax * k)} ${r(y1 + ay * k)} ${r(x2 + bx * k)} ${r(y2 + by * k)} ${r(x2)} ${r(y2)}`;
}
