// SPDX-License-Identifier: Apache-2.0
/*
 * The GroomLab engine in the browser: the core built to WebAssembly (engine/web, docs/WEB.md),
 * running in a worker (public/wasm/groomlab-worker.js) so evaluating never blocks the page. Load a
 * .groom, list its sliders, set them, evaluate at a preview density and save the edited file.
 * Nothing here is fetched until createGroomEngine() is called: pages that never ask never pay.
 */
import type { HairLook, RootMesh, StrandData } from '@/components/home/strand-data';

/** One slider: a Control node's value, or a node param exposed by the groom. */
export interface GroomControl {
  /** "control:<name>" or "param:<node>.<param>": what setParam takes. */
  id: string;
  /** control: a Control node; float or int: a node param. */
  kind: 'control' | 'float' | 'int';
  label: string;
  /** What it belongs to: "Controls", or the node ("Clump (clump)"). */
  group: string;
  node: string;
  param: string;
  min: number;
  max: number;
  step: number;
  default: number;
  value: number;
  /** The param's unit: "length", "width", "density", "angle"... ("" none). */
  unit: string;
  description: string;
}

/** A file the groom reads: a texture map (a map param's, a painted texture map's) or a referenced groom. */
export interface GroomFileRef {
  /** As the groom stores it (relative to the .groom's folder, or absolute). */
  path: string;
  /** Where the engine reads it, in the module's memory file system ("/groom" stands for the .groom's folder). */
  resolved: string;
  found: boolean;
  kind: 'texture' | 'groom';
  /** A tile set (<UDIM>, or a 1001-style tile name): every tile of it is read. */
  udim: boolean;
  /** What reads it: "<node>.<param>" or "map <name>". */
  users: string[];
}

/** Files for the engine: path (relative to the .groom's folder, or a plain name) to bytes. */
export type GroomFiles = Record<string, ArrayBuffer | Uint8Array> | Iterable<[string, ArrayBuffer | Uint8Array]>;

export interface GroomFileState {
  files: GroomFileRef[];
  /** The stored paths of the files still missing: ask for them. */
  missing: string[];
  /** Files that could not be written, with why. */
  failed: string[];
}

export interface GroomLoaded extends GroomFileState {
  /** The engine's version (GroomLab's). */
  version: string;
  loadMs: number;
  /** How long the load waited for the WebAssembly module (the worker's first load: fetching and compiling it). */
  moduleMs: number;
  controls: GroomControl[];
  scalp: RootMesh;
}

export interface GroomEvaluation {
  strands: StrandData;
  /** The guides as drawn (the flat guides, else the hierarchy's); null without any. */
  guides: StrandData | null;
  /** The engine's own time for the evaluation, and the worker's round trip (copies included). */
  evaluateMs: number;
  totalMs: number;
  /** Why the evaluation was partial, line by line (empty: it was not). */
  errors: string[];
  /** Milliseconds per node in this evaluation (nodes from cache take none). */
  nodes: { node: string; ms: number }[];
  /** The hair's highlight as Maya's viewport shades it (null: no groom). */
  look: HairLook | null;
}

export interface GroomEngine {
  /**
   * A .groom (its scalp inside), or a preset's graph with an .obj to grow on, with the files it reads
   * (texture maps: see addFile). The files of the groom loaded before are dropped.
   */
  loadGroom(groom: string | ArrayBuffer | Uint8Array, mesh?: string | ArrayBuffer | Uint8Array, files?: GroomFiles): Promise<GroomLoaded>;
  /**
   * A file the groom reads, read by the engine's own loaders as the desktop reads it from disk.
   * `path` is relative to the .groom's folder ("masks/manny_hair/highlights.png", "../maps/x.png"),
   * or just a name: a file whose name is a missing file's (any case; a UDIM set's tiles by their
   * pattern) stands for it wherever the groom looks for it. Evaluate again to see it.
   */
  addFile(path: string, bytes: ArrayBuffer | Uint8Array): Promise<GroomFileState>;
  addFiles(files: GroomFiles): Promise<GroomFileState>;
  /** The stored paths of the files the groom reads and nobody provided. */
  missingFiles(): Promise<string[]>;
  /** Every file the groom reads, found or not. */
  referencedFiles(): Promise<GroomFileRef[]>;
  listControls(): Promise<GroomControl[]>;
  /** Sets sliders (only the latest value of each is sent while an evaluation runs). */
  setParam(id: string, value: number): Promise<void>;
  setParams(values: [string, number][]): Promise<void>;
  /** Evaluates at `density` (0-1; the preview density, a share of the strands). */
  evaluate(options?: { density?: number }): Promise<GroomEvaluation>;
  /** The scalp as evaluated (it can change with the graph: a Scalp node's subdivision). */
  scalpMesh(): Promise<RootMesh>;
  /** The groom as a .groom file, with the edits made. */
  saveGroom(): Promise<Uint8Array>;
  /** Bypasses nodes, or puts them back ([node name, bypassed]): the graph's own bypass flag. */
  setBypass(nodes: [string, boolean][]): Promise<void>;
  /**
   * Another groom in the same worker (one engine, several grooms: the home page's viewers), its
   * calls the same; `name` is its slot. Disposing it unloads that groom only.
   */
  slot(name: string): GroomEngine;
  /** The main engine: ends the worker (every slot). A slot: unloads its groom. */
  dispose(): void;
}

/** The density evaluate uses when given none: a quick preview. */
export const DEFAULT_PREVIEW_DENSITY = 0.25;

const text = (v: string | ArrayBuffer | Uint8Array | undefined) =>
  v === undefined ? '' : typeof v === 'string' ? v : new TextDecoder().decode(v);

const fileList = (files: GroomFiles | undefined): [string, ArrayBuffer | Uint8Array][] =>
  !files ? [] : Symbol.iterator in files ? [...(files as Iterable<[string, ArrayBuffer | Uint8Array]>)] : Object.entries(files);

interface RawEvaluation {
  count: number;
  offsets: Uint32Array;
  positions: Float32Array;
  colors: Float32Array;
  widths: Float32Array;
  guides: { offsets: Uint32Array; positions: Float32Array } | null;
  evaluateMs: number;
  totalMs: number;
  errors: string[];
  nodes: { node: string; ms: number }[];
  look: HairLook | null;
}

/**
 * The missing files of a groom loaded by URL, fetched relative to it as the desktop reads them
 * relative to the .groom's folder: GroomFiles for addFiles. Relative paths only (an absolute path or
 * one holding a variable is another machine's); a UDIM set's tiles cannot be listed over HTTP, so
 * its pattern is left out; files the server does not have are too.
 */
export async function fetchGroomFiles(missing: string[], groomUrl: string | URL): Promise<[string, Uint8Array][]> {
  const base = new URL(groomUrl, typeof location === 'undefined' ? undefined : location.href);
  const wanted = missing.filter((p) => !/^([a-z]:)?[\\/]|[$%]|<udim>|%\(udim\)d/i.test(p));
  const got = await Promise.all(
    wanted.map(async (path): Promise<[string, Uint8Array] | null> => {
      try {
        const response = await fetch(new URL(path.replace(/\\/g, '/'), base));
        return response.ok ? [path, new Uint8Array(await response.arrayBuffer())] : null;
      } catch {
        return null;
      }
    }),
  );
  return got.filter((f): f is [string, Uint8Array] => f !== null);
}

type Call = <T>(op: string, args?: Record<string, unknown>) => Promise<T>;

/** The calls of one slot of the worker, with its own queue of slider values. */
function slotApi(call: Call, slot: string, slotOf: (name: string) => GroomEngine, end: () => void): GroomEngine {
  const at = <T>(op: string, args: Record<string, unknown> = {}) => call<T>(op, { ...args, slot });
  // Slider values wait here while a set is in flight; the latest of each goes next.
  const queued = new Map<string, number>();
  let flushing: Promise<void> | null = null;
  const flush = async () => {
    while (queued.size) {
      const values = [...queued.entries()];
      queued.clear();
      await at<{ failed: string[] }>('set', { values });
    }
  };
  const setParams = (values: [string, number][]) => {
    for (const [id, v] of values) queued.set(id, v);
    if (!flushing) flushing = flush().finally(() => (flushing = null));
    return flushing;
  };
  return {
    loadGroom: (groom, mesh, files) => at<GroomLoaded>('load', { groom: text(groom), mesh: text(mesh), files: fileList(files) }),
    addFile: (path, bytes) => at<GroomFileState>('addFiles', { files: [[path, bytes]] }),
    addFiles: (files) => at<GroomFileState>('addFiles', { files: fileList(files) }),
    missingFiles: async () => (await at<GroomFileState>('files')).missing,
    referencedFiles: async () => (await at<GroomFileState>('files')).files,
    listControls: () => at<GroomControl[]>('controls'),
    setParam: (id, value) => setParams([[id, value]]),
    setParams,
    async setBypass(nodes) {
      if (flushing) await flushing;
      await at('bypass', { nodes });
    },
    async evaluate(options) {
      if (flushing) await flushing;
      const r = await at<RawEvaluation>('evaluate', { density: options?.density ?? DEFAULT_PREVIEW_DENSITY });
      return {
        strands: { count: r.count, offsets: r.offsets, positions: r.positions, colors: r.colors, widths: r.widths, density: options?.density ?? DEFAULT_PREVIEW_DENSITY },
        guides: r.guides ? { count: r.guides.offsets.length - 1, offsets: r.guides.offsets, positions: r.guides.positions } : null,
        evaluateMs: r.evaluateMs,
        totalMs: r.totalMs,
        errors: r.errors,
        nodes: r.nodes,
        look: r.look ?? null,
      };
    },
    scalpMesh: () => at<RootMesh>('scalp'),
    saveGroom: async () => new TextEncoder().encode(await at<string>('save')),
    slot: slotOf,
    dispose: end,
  };
}

/** Starts the engine's worker (`base`: where groomlab-worker.js, groomlab.js and .wasm are served). */
export function createGroomEngine(base = '/wasm/'): GroomEngine {
  const worker = new Worker(`${base}groomlab-worker.js`);
  let next = 1;
  let ended = false;
  const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();
  worker.onmessage = (e: MessageEvent<{ id: number; ok: boolean; result?: unknown; error?: string }>) => {
    const p = pending.get(e.data.id);
    if (!p) return;
    pending.delete(e.data.id);
    if (e.data.ok) p.resolve(e.data.result);
    else p.reject(new Error(e.data.error));
  };
  worker.onerror = (e) => {
    for (const p of pending.values()) p.reject(new Error(e.message || 'the GroomLab worker failed to start'));
    pending.clear();
  };
  const call: Call = <T>(op: string, args?: Record<string, unknown>) =>
    new Promise<T>((resolve, reject) => {
      if (ended) return reject(new Error('the GroomLab engine was disposed'));
      const id = next++;
      pending.set(id, { resolve: resolve as (v: unknown) => void, reject });
      worker.postMessage({ id, op, args });
    });

  const slots = new Map<string, GroomEngine>();
  const slotOf = (name: string): GroomEngine => {
    if (name === 'main') return main;
    let s = slots.get(name);
    if (!s) {
      s = slotApi(call, name, slotOf, () => {
        slots.delete(name);
        call('unload', { slot: name }).catch(() => {});
      });
      slots.set(name, s);
    }
    return s;
  };
  const main = slotApi(call, 'main', slotOf, () => {
    ended = true;
    worker.terminate();
    for (const p of pending.values()) p.reject(new Error('the GroomLab engine was disposed'));
    pending.clear();
  });
  return main;
}
