// SPDX-License-Identifier: Apache-2.0
/*
 * The home page's grooms grown by the real GroomLab engine: the core built to WebAssembly
 * (docs/WEB.md) in one worker for the whole page, each viewer's groom in its own slot of it. Fetched
 * only when a 3D viewport has mounted (strand-3d.ts imports this lazily), so the page's first load
 * never pays for it. Evaluation runs in the worker: the page's thread only copies the result into
 * the viewer. Edits (sliders, bypass, a new seed) go to the engine as they come; evaluations are
 * one at a time and at most one a frame, always with the latest edits.
 */
import { createGroomEngine, type GroomControl, type GroomEngine } from '@/lib/groomlab-wasm';
import type { EngineEdits, EngineSource } from './engine-grooms';
import type { HairLook, RootMesh, StrandData } from './strand-data';

/** Engine space (the Manny head in centimetres) to the viewers' groom space (head-mesh.ts parseHead's). */
const MANNY_SPACE: NonNullable<EngineSource['space']> = { center: [0, 3.1, -0.4], scale: 0.1 };

function toGroomSpace(positions: Float32Array, space: NonNullable<EngineSource['space']>) {
  const { center, scale } = space;
  for (let i = 0; i < positions.length; i += 3) {
    positions[i] = (positions[i] - center[0]) * scale;
    positions[i + 1] = (positions[i + 1] - center[1]) * scale;
    positions[i + 2] = (positions[i + 2] - center[2]) * scale;
  }
  return positions;
}

let shared: GroomEngine | null = null;
/** Slots are numbered per viewer, so a viewer mounted again never shares one with its old self. */
let slots = 0;

/** The page's one engine (its worker starts on the first call). Null without WebAssembly or workers. */
function engine(): GroomEngine | null {
  if (shared) return shared;
  if (typeof WebAssembly !== 'object' || typeof Worker !== 'function') return null;
  shared = createGroomEngine();
  return shared;
}

export interface EngineView {
  strands: StrandData;
  /** The scalp painted with the hair's root color under the hair; only when it changed. */
  scalp: RootMesh | null;
  /** The guides, for the viewer to draw when they are shown (null: the groom has none). */
  guides: StrandData | null;
  /** The hair's highlight (the groom's HairColor's), for the viewer's shading. */
  look: HairLook | null;
  stats: { strands: number; guides: number; evaluateMs: number; totalMs: number };
}

export interface EngineGroom {
  /** Sets these edits (only what changed goes to the engine) and evaluates again if any did. */
  edit(edits: EngineEdits): void;
  dispose(): void;
}

export interface EngineGroomOptions {
  source: EngineSource;
  /** Share of `source.strands` to draw (phones: fewer). */
  strandScale: number;
  /** The head's own color, sRGB 0 to 1. */
  skin: number[];
  edits: EngineEdits;
  onView(view: EngineView): void;
  /** The groom's sliders, as the engine lists them, once it has loaded. */
  onControls?(controls: GroomControl[]): void;
  /** The engine could not grow this groom: the viewer keeps what it had. */
  onFail?(error: unknown): void;
  /**
   * Timings for the page's perf probe: milestones (fetchGroom, loadGroom, firstEvaluate) as
   * performance.now(), durations (names ending in Ms) in milliseconds.
   */
  onTiming?(name: string, ms: number): void;
}

const smooth = (e: number) => {
  const t = Math.min(1, Math.max(0, e));
  return t * t * (3 - 2 * t);
};

/** sRGB 0-1 to linear and back. */
const toLinear = (c: number) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
const toSrgb = (c: number) => (c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
/**
 * The color hair of base color `srgb` looks (core's hairAppearanceColor, hair_color.h: its absorption
 * as aiStandardHair takes a base color, at the default azimuthal roughness, through the appearance
 * fit), sRGB.
 */
function hairAppearance(srgb: number): number {
  const b = 0.2;  // kHairAzimuthalRoughness
  const b2 = b * b;
  const d = 5.969 - 0.215 * b + 2.532 * b2 - 10.73 * b2 * b + 5.574 * b2 * b2 + 0.245 * b2 * b2 * b;
  const l = Math.log(Math.min(Math.max(toLinear(srgb), 1e-8), 1)) / d;
  const sigma = l * l;
  return toSrgb(0.03 + 0.97 * Math.exp(-3.52 * Math.pow(sigma, 0.79)));
}

/**
 * The scalp colored as a groom's scalp texture would be: the color the strands nearby look (their
 * base color's appearance), over the skin, so the hair reads full with a preview's share of the
 * strands. Both in
 * engine space (cm); roots are bucketed in a grid of `cell` cm.
 */
function paintScalp(scalp: RootMesh, strands: StrandData, skin: number[]): Float32Array {
  const cell = 1.2;
  const reach = 1.1;
  const { count, offsets, positions, colors } = strands;
  const grid = new Map<number, number[]>();
  const key = (cx: number, cy: number, cz: number) => (cx + 512) * 1048576 + (cy + 512) * 1024 + (cz + 512);
  for (let i = 0; i < count; i++) {
    const p = offsets[i] * 3;
    const k = key(Math.floor(positions[p] / cell), Math.floor(positions[p + 1] / cell), Math.floor(positions[p + 2] / cell));
    const list = grid.get(k);
    if (list) list.push(i);
    else grid.set(k, [i]);
  }
  const out = new Float32Array(scalp.positions.length);
  const v = scalp.positions;
  for (let q = 0; q < v.length; q += 3) {
    const x = v[q];
    const y = v[q + 1];
    const z = v[q + 2];
    const cx = Math.floor(x / cell);
    const cy = Math.floor(y / cell);
    const cz = Math.floor(z / cell);
    let w = 0;
    let r = 0;
    let g = 0;
    let b = 0;
    let near = 0;
    for (let dx = -1; dx <= 1; dx++)
      for (let dy = -1; dy <= 1; dy++)
        for (let dz = -1; dz <= 1; dz++) {
          const list = grid.get(key(cx + dx, cy + dy, cz + dz));
          if (!list) continue;
          for (const i of list) {
            const p = offsets[i] * 3;
            const d = Math.hypot(positions[p] - x, positions[p + 1] - y, positions[p + 2] - z);
            if (d > reach) continue;
            const k = 1 - d / reach;
            w += k;
            near++;
            if (colors) {
              r += colors[p] * k;
              g += colors[p + 1] * k;
              b += colors[p + 2] * k;
            } else {
              r += 0.4 * k;
              g += 0.4 * k;
              b += 0.4 * k;
            }
          }
        }
    // A few roots around this vertex: under the hair, the color it looks, in its shadow.
    const t = smooth(near / 3);
    const shade = 0.2;
    const root = w > 0 ? [hairAppearance(r / w) * shade, hairAppearance(g / w) * shade, hairAppearance(b / w) * shade] : skin;
    for (let c = 0; c < 3; c++) out[q + c] = skin[c] + (root[c] - skin[c]) * t;
  }
  return out;
}

/** A viewer's groom, grown by the engine: loads `source`, evaluates it, and again on each edit. */
export function attachEngineGroom(opts: EngineGroomOptions): EngineGroom {
  const timing = (name: string) => opts.onTiming?.(name, performance.now());
  const root = engine();
  const { source } = opts;
  const space = source.space ?? MANNY_SPACE;
  let disposed = false;
  let slot: GroomEngine | null = null;
  /** What the engine has; what the page wants. */
  const applied = { params: {} as Record<string, number>, bypass: {} as Record<string, boolean> };
  let wanted: EngineEdits = opts.edits;
  let running = false;
  let dirty = false;
  let raf = 0;
  let scalp: RootMesh | null = null;
  let scalpPainted = '';

  const fail = (e: unknown) => {
    if (disposed) return;
    disposed = true;
    slot?.dispose();
    slot = null;
    opts.onFail?.(e);
  };
  if (!root) {
    queueMicrotask(() => fail(new Error('no WebAssembly')));
    return { edit() {}, dispose() {} };
  }

  /** The preview density: about `strands` of the groom's, more with its density param, up to the cap. */
  const density = () => {
    const target = source.strands * opts.strandScale;
    let full = source.fullStrands;
    if (source.densityParam) {
      const [id, base] = source.densityParam;
      full *= Math.max(0, wanted.params?.[id] ?? base) / base;
    }
    const d = target / source.fullStrands;
    const cap = (source.maxStrands ? source.maxStrands * opts.strandScale : target) / Math.max(1, full);
    return Math.max(1e-4, Math.min(1, d, cap));
  };

  /** Sends what changed since the engine last heard. */
  const push = async (g: GroomEngine) => {
    const params: [string, number][] = [];
    for (const [id, v] of Object.entries(wanted.params ?? {})) if (applied.params[id] !== v) params.push([id, v]);
    const bypass: [string, boolean][] = [];
    for (const [node, on] of Object.entries(wanted.bypass ?? {})) if ((applied.bypass[node] ?? false) !== on) bypass.push([node, on]);
    for (const [id, v] of params) applied.params[id] = v;
    for (const [node, on] of bypass) applied.bypass[node] = on;
    if (params.length) await g.setParams(params);
    if (bypass.length) await g.setBypass(bypass);
  };

  const run = async () => {
    raf = 0;
    const g = slot;
    if (!g || disposed) return;
    if (running) {
      dirty = true;
      return;
    }
    running = true;
    try {
      do {
        dirty = false;
        await push(g);
        const first = !scalp;
        const r = await g.evaluate({ density: density() });
        if (disposed) return;
        if (first) {
          timing('firstEvaluate');
          opts.onTiming?.('firstEvaluateEngineMs', r.evaluateMs);
          scalp = await g.scalpMesh();
          if (disposed) return;
        }
        // The scalp's paint follows the roots and their color: again when either changed.
        const sig = `${r.strands.count}:${r.strands.colors?.[0]}:${r.strands.positions[0]}:${r.strands.colors?.[r.strands.colors.length - 1]}`;
        let painted: RootMesh | null = null;
        if (scalp && sig !== scalpPainted) {
          scalpPainted = sig;
          const p0 = performance.now();
          const colors = paintScalp(scalp, r.strands, opts.skin);
          opts.onTiming?.('paintMs', performance.now() - p0);
          painted = {
            positions: first ? toGroomSpace(scalp.positions, space) : scalp.positions,
            normals: scalp.normals && scalp.normals.length === scalp.positions.length ? scalp.normals : undefined,
            indices: scalp.indices,
            colors,
          };
        }
        toGroomSpace(r.strands.positions, space);
        if (r.strands.widths) for (let i = 0; i < r.strands.widths.length; i++) r.strands.widths[i] *= space.scale;
        if (r.guides) toGroomSpace(r.guides.positions, space);
        opts.onView({
          strands: r.strands,
          scalp: painted,
          guides: r.guides,
          look: r.look,
          stats: { strands: r.strands.count, guides: r.guides?.count ?? 0, evaluateMs: r.evaluateMs, totalMs: r.totalMs },
        });
        opts.onTiming?.('evaluateMs', r.evaluateMs);
        opts.onTiming?.('roundTripMs', r.totalMs);
        if (r.errors.length) console.warn(`GroomLab (${source.slot}):`, r.errors.join('\n'));
      } while (dirty && !disposed);
    } catch (e) {
      fail(e);
    } finally {
      running = false;
    }
  };
  const schedule = () => {
    if (running) dirty = true;
    else if (!raf) raf = requestAnimationFrame(() => void run());
  };

  (async () => {
    const g = root.slot(`${source.slot}-${++slots}`);
    const response = await fetch(source.url);
    if (!response.ok) throw new Error(`${source.url}: ${response.status}`);
    const text = await response.text();
    timing('fetchGroom');
    if (disposed) return;
    const loaded = await g.loadGroom(text);
    timing('loadGroom');
    opts.onTiming?.('moduleMs', loaded.moduleMs);
    opts.onTiming?.('loadMs', loaded.loadMs);
    if (disposed) {
      g.dispose();
      return;
    }
    slot = g;
    opts.onControls?.(loaded.controls);
    schedule();
  })().catch(fail);

  return {
    edit(e) {
      const changed =
        Object.entries(e.params ?? {}).some(([id, v]) => applied.params[id] !== v) ||
        Object.entries(e.bypass ?? {}).some(([node, on]) => (applied.bypass[node] ?? false) !== on);
      wanted = e;
      if (changed && slot && !disposed) schedule();
    },
    dispose() {
      disposed = true;
      cancelAnimationFrame(raf);
      slot?.dispose();
      slot = null;
    },
  };
}
