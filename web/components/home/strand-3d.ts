// SPDX-License-Identifier: Apache-2.0
/*
 * The lazily loaded half of StrandViewport: a groom wired to the three.js viewer, on the Manny head.
 * This module (and three.js with it) is only fetched when a viewport scrolls near the screen; the
 * head mesh (a few kB) is fetched with it, once per page. First it draws the bare head; when the
 * page is idle it fetches the real GroomLab engine (home-engine.ts: WebAssembly, in a worker) and
 * shows the engine's strands, scalp and guides once they come, in the same canvas. It stands still:
 * the engine grows it again only when its edits change, and nothing renders unless it is dragged,
 * resized or changed.
 */
import type { GroomControl } from '@/lib/groomlab-wasm';
import type { EngineEdits, EngineSource } from './engine-grooms';
import { headRootMesh, loadHead } from './head-mesh';
import type { EngineGroom } from './home-engine';
import type { StrandData } from './strand-data';
import { createStrandViewer, type ViewerOptions } from './strand-three';

/** What a viewport's groom has drawn. */
export interface GroomStats {
  strands: number;
  guides: number;
}

/** The real groom a viewport shows once the engine has grown it. */
export interface EngineOptions {
  source: EngineSource;
  edits: EngineEdits;
  onControls?: (controls: GroomControl[]) => void;
  /** Share of the source's strands to draw (phones: fewer). */
  scale?: number;
  /** Called once the engine's strands are on screen. */
  onReady?: () => void;
}

/**
 * The page's engine timings, for its perf probe (window.__groomlabHome): per slot, milestones as
 * milliseconds since the page started, and per evaluation the engine's time, the worker's round trip
 * and the time the page's thread took to hand the result to the viewer.
 */
type Timings = Record<string, Record<string, number | number[]>>;
function record(slot: string, name: string, ms = performance.now()) {
  const w = window as unknown as { __groomlabHome?: Timings };
  const t = ((w.__groomlabHome ??= {})[slot] ??= {});
  if (name === 'evaluateMs' || name === 'roundTripMs' || name === 'applyMs' || name === 'paintMs') ((t[name] ??= []) as number[]).push(Math.round(ms * 10) / 10);
  else t[name] = Math.round(ms);
}

export interface Groom3DOptions extends Omit<ViewerOptions, 'onFrame' | 'spin' | 'sway'> {
  /** The head's clay color, sRGB 0 to 1. */
  skin?: number[];
  /** The sphere the camera frames: center and radius, in head units (the cranium ~1 across). */
  framing?: [[number, number, number], number];
  showGuides?: boolean;
  onStats?: (stats: GroomStats) => void;
  /** The groom the engine grows. */
  engine: EngineOptions;
}

export interface Groom3D {
  /** The engine's groom: these edits (sent once it has loaded). */
  edit(edits: EngineEdits): void;
  showGuides(on: boolean): void;
  setActive(active: boolean): void;
  resize(width: number, height: number): void;
  invalidate(): void;
  dispose(): void;
}

/** The 3D groom on `canvas`, once its head has loaded; null without WebGL. */
export async function mountGroom3D(canvas: HTMLCanvasElement, opts: Groom3DOptions): Promise<Groom3D | null> {
  const head = await loadHead();
  const viewer = createStrandViewer(canvas, { ...opts, spin: 0, sway: 0 });
  if (!viewer) return null;
  // The head and the hair over it, a little above center.
  const [center, radius] = opts.framing ?? [[0, 0.1, 0], 1.45];
  viewer.setFraming(center, radius);
  const skin = opts.skin ?? [0.7, 0.68, 0.66];
  // The bare head, until the engine's scalp comes (a groom on another mesh shows nothing until then).
  if (head && opts.engine.source.bareHead !== false) {
    viewer.setScalp(
      headRootMesh(head, (_x, _y, _z, out) => {
        out[0] = skin[0];
        out[1] = skin[1];
        out[2] = skin[2];
      }),
    );
  }

  let guidesOn = opts.showGuides ?? false;
  let realGuides: StrandData | null = null;
  let shownGuides: StrandData | null | undefined;
  const showGuides = () => {
    const g = guidesOn ? realGuides : null;
    if (g !== shownGuides) {
      shownGuides = g;
      viewer.setGuides(g);
    }
  };

  // The engine, once the page is idle: its worker and WebAssembly are fetched then, once a page.
  let engine: EngineGroom | null = null;
  let disposed = false;
  let ready = false;
  let edits = opts.engine.edits;
  const startEngine = () => {
    const e = opts.engine;
    if (disposed) return;
    const slot = e.source.slot;
    record(slot, 'engineAsked');
    import('./home-engine')
      .then(({ attachEngineGroom }) => {
        if (disposed) return;
        engine = attachEngineGroom({
          source: e.source,
          strandScale: e.scale ?? 1,
          skin,
          edits,
          onControls: e.onControls,
          onTiming: (name, ms) => record(slot, name, ms),
          onFail: (err) => console.warn(`GroomLab engine (${slot}) failed:`, err),
          onView(view) {
            if (disposed) return;
            if (!ready) {
              ready = true;
              record(slot, 'onScreen');
              e.onReady?.();
            }
            const t0 = performance.now();
            if (view.look) viewer.setLook(view.look);
            viewer.setStrands(view.strands);
            if (view.scalp) viewer.setScalp(view.scalp);
            realGuides = view.guides;
            shownGuides = undefined;
            showGuides();
            record(slot, 'applyMs', performance.now() - t0);
            opts.onStats?.({ strands: view.stats.strands, guides: view.stats.guides });
          },
        });
      })
      .catch((err) => console.warn('GroomLab engine failed:', err));
  };
  const w = window as Window & { requestIdleCallback?: (cb: () => void, o?: { timeout: number }) => number };
  const idle = w.requestIdleCallback ? w.requestIdleCallback(startEngine, { timeout: 1200 }) : window.setTimeout(startEngine, 200);

  return {
    edit(e) {
      edits = e;
      engine?.edit(e);
    },
    showGuides(on) {
      guidesOn = on;
      showGuides();
    },
    setActive: viewer.setActive,
    resize: viewer.resize,
    invalidate: viewer.invalidate,
    dispose() {
      disposed = true;
      const w = window as Window & { cancelIdleCallback?: (id: number) => void };
      w.cancelIdleCallback?.(idle);
      clearTimeout(idle);
      engine?.dispose();
      viewer.dispose();
    },
  };
}
