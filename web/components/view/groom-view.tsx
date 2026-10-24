// SPDX-License-Identifier: Apache-2.0
'use client';
import { useCallback, useEffect, useId, useRef, useState, type CSSProperties, type DragEvent } from 'react';
import { ArrowDownToLine, FolderOpen, FolderTree } from 'lucide-react';
import { category } from '@/components/home/graph-theme';
import type { RootMesh, StrandData } from '@/components/home/strand-data';
import type { StrandViewer } from '@/components/home/strand-three';
import type { GroomControl, GroomEngine } from '@/lib/groomlab-wasm';
import { cn } from '@/lib/cn';

interface Stats {
  strands: number;
  points: number;
  evaluateMs: number;
  totalMs: number;
}

/** The view's space: the scalp's bounding sphere moved to the origin and scaled to radius 1. */
interface Space {
  center: [number, number, number];
  scale: number;
}

function sphereOf(positions: Float32Array): Space {
  const lo = [Infinity, Infinity, Infinity];
  const hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < positions.length; i += 3) {
    for (let k = 0; k < 3; k++) {
      lo[k] = Math.min(lo[k], positions[i + k]);
      hi[k] = Math.max(hi[k], positions[i + k]);
    }
  }
  const center: [number, number, number] = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
  const r = Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) / 2;
  return { center, scale: r > 0 ? 1 / r : 1 };
}

function toSpace(positions: Float32Array, s: Space) {
  for (let i = 0; i < positions.length; i += 3) {
    positions[i] = (positions[i] - s.center[0]) * s.scale;
    positions[i + 1] = (positions[i + 1] - s.center[1]) * s.scale;
    positions[i + 2] = (positions[i + 2] - s.center[2]) * s.scale;
  }
  return positions;
}

function decimals(step: number) {
  return step >= 1 ? 0 : Math.min(4, Math.max(0, Math.ceil(-Math.log10(step))));
}

/** A file picked or dropped, with its path in what was picked (a folder's files: "hair/masks/x.png"). */
interface PickedFile {
  path: string;
  file: File;
}

/** Image files the engine reads (texture.h isImagePath), and grooms (a Reference node's). */
const READABLE = /\.(png|jpe?g|tga|bmp|psd|gif|hdr|pic|pnm|ppm|pgm|groom)$/i;
const IMAGE_ACCEPT = '.png,.jpg,.jpeg,.tga,.bmp,.psd,.gif,.hdr,.pic,.pnm,.ppm,.pgm';

const pickedOf = (files: FileList | File[]): PickedFile[] =>
  [...files].map((file) => ({ path: file.webkitRelativePath || file.name, file }));

/** `path` relative to the folder `from` (both "/"-separated, relative to what was picked). */
function relativeTo(from: string, path: string) {
  const a = from ? from.split('/') : [];
  const b = path.split('/');
  let i = 0;
  while (i < a.length && i < b.length - 1 && a[i] === b[i]) i++;
  return [...a.slice(i).map(() => '..'), ...b.slice(i)].join('/');
}

/** Every file under the dropped items, folders walked (their paths kept). Call during the drop. */
function droppedFiles(data: DataTransfer): Promise<PickedFile[]> {
  const entries = [...data.items].map((item) => item.webkitGetAsEntry?.()).filter((e): e is FileSystemEntry => !!e);
  if (!entries.length) return Promise.resolve(pickedOf(data.files));
  const walk = async (entry: FileSystemEntry): Promise<PickedFile[]> => {
    const path = entry.fullPath.replace(/^\//, '');
    if (entry.isFile) {
      const file = await new Promise<File>((resolve, reject) => (entry as FileSystemFileEntry).file(resolve, reject));
      return [{ path, file }];
    }
    const reader = (entry as FileSystemDirectoryEntry).createReader();
    const children: FileSystemEntry[] = [];
    for (;;) {
      const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
      if (!batch.length) break;
      children.push(...batch);
    }
    return (await Promise.all(children.map(walk))).flat();
  };
  return Promise.all(entries.map(walk)).then((lists) => lists.flat());
}

/** The files the engine may read, as paths relative to the groom's folder (`groomPath`'s) and bytes. */
async function engineFiles(picked: PickedFile[], groomPath: string | null): Promise<[string, Uint8Array][]> {
  const folder = groomPath && groomPath.includes('/') ? groomPath.slice(0, groomPath.lastIndexOf('/')) : '';
  return Promise.all(
    picked
      .filter((p) => p.path !== groomPath && READABLE.test(p.path))
      .map(async (p): Promise<[string, Uint8Array]> => [relativeTo(folder, p.path), new Uint8Array(await p.file.arrayBuffer())]),
  );
}

/** A bundled sample named by the page's address (?groom=manny_curls: the home page's link), else null. */
function sampleFromAddress() {
  const name = new URLSearchParams(window.location.search).get('groom');
  return name && /^[a-z0-9_]{1,64}$/.test(name) ? `/samples/${name}.groom` : null;
}

/** True when this browser can make a WebGL2 context (three.js needs one). */
function hasWebGL2() {
  try {
    const gl = document.createElement('canvas').getContext('webgl2');
    gl?.getExtension('WEBGL_lose_context')?.loseContext();
    return !!gl;
  } catch {
    return false;
  }
}

/**
 * The groom viewer (/view): the GroomLab engine built to WebAssembly (engine/web) evaluating a
 * .groom in a worker, drawn by the home page's three.js viewer, with the groom's sliders (its
 * Control nodes, then its exposed params) re-evaluated live. Everything heavy (three.js, the
 * engine's 1 MB of WebAssembly) is fetched by this page alone, after it mounts.
 */
export function GroomView({ sample }: { sample: string }) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const viewerRef = useRef<StrandViewer | null>(null);
  const engineRef = useRef<GroomEngine | null>(null);
  const spaceRef = useRef<Space | null>(null);
  const framedRef = useRef(false);
  const densityRef = useRef(0.25);
  const evalRef = useRef<{ running: boolean; dirty: boolean }>({ running: false, dirty: false });

  const [status, setStatus] = useState('Loading the engine…');
  const [error, setError] = useState<string | null>(null);
  const [warnings, setWarnings] = useState<string[]>([]);
  const [controls, setControls] = useState<GroomControl[]>([]);
  const [values, setValues] = useState<Record<string, number>>({});
  const [density, setDensity] = useState(0.25);
  const [stats, setStats] = useState<Stats | null>(null);
  const [version, setVersion] = useState('');
  const [fileName, setFileName] = useState('webhero.groom');
  const [dragging, setDragging] = useState(false);
  const [ready, setReady] = useState(false);
  /** The files the groom reads that nobody provided (texture maps): the notice asks for them. */
  const [missing, setMissing] = useState<string[]>([]);

  /** Evaluates now, or once more after the evaluation running (the latest values win). */
  const evaluate = useCallback(async () => {
    const engine = engineRef.current;
    const state = evalRef.current;
    if (!engine) return;
    if (state.running) {
      state.dirty = true;
      return;
    }
    state.running = true;
    try {
      do {
        state.dirty = false;
        const r = await engine.evaluate({ density: densityRef.current });
        const space = spaceRef.current;
        const viewer = viewerRef.current;
        if (space && viewer) {
          const strands: StrandData = { ...r.strands, positions: toSpace(r.strands.positions, space) };
          if (strands.widths) for (let i = 0; i < strands.widths.length; i++) strands.widths[i] *= space.scale;
          if (r.look) viewer.setLook(r.look);
          viewer.setStrands(strands);
          viewer.setGuides(null);
          if (!framedRef.current) {
            framedRef.current = true;
            // The hair and the scalp (radius 1 about the origin) together.
            const s = sphereOf(strands.positions);
            const lo = s.center.map((c) => Math.min(c - 1 / s.scale, -1));
            const hi = s.center.map((c) => Math.max(c + 1 / s.scale, 1));
            const mid: [number, number, number] = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
            viewer.setFraming(mid, Math.max(...hi.map((h, k) => (h - lo[k]) / 2)));
          }
        }
        setStats({ strands: r.strands.count, points: r.strands.offsets[r.strands.count], evaluateMs: r.evaluateMs, totalMs: r.totalMs });
        setWarnings(r.errors);
        setError(null);
      } while (state.dirty);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      state.running = false;
    }
  }, []);

  /**
   * Loads a groom (a .groom's text, or a preset's with an .obj) into the engine and the viewer, with
   * the files it reads (texture maps, relative to its folder); one loaded by URL fetches the maps it
   * still misses relative to that URL, as the desktop reads them beside the .groom.
   */
  const load = useCallback(
    async (groom: string, mesh: string | undefined, name: string, files: [string, Uint8Array][] = [], url?: string) => {
      const engine = engineRef.current;
      if (!engine) return;
      setStatus(`Loading ${name}…`);
      try {
        const loaded = await engine.loadGroom(groom, mesh, files);
        let { missing: stillMissing } = loaded;
        if (url && stillMissing.length) {
          const { fetchGroomFiles } = await import('@/lib/groomlab-wasm');
          const fetched = await fetchGroomFiles(stillMissing, url);
          if (fetched.length) stillMissing = (await engine.addFiles(fetched)).missing;
        }
        setMissing(stillMissing);
        const scalp: RootMesh = loaded.scalp;
        const space = sphereOf(scalp.positions);
        spaceRef.current = space;
        framedRef.current = false;
        viewerRef.current?.setScalp({
          positions: toSpace(scalp.positions, space),
          normals: scalp.normals && scalp.normals.length === scalp.positions.length ? scalp.normals : undefined,
          indices: scalp.indices,
        });
        setVersion(loaded.version);
        setControls(loaded.controls);
        setValues(Object.fromEntries(loaded.controls.map((c) => [c.id, c.value])));
        setFileName(name);
        setError(null);
        setStatus('');
        await evaluate();
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        setStatus('');
      }
    },
    [evaluate],
  );

  useEffect(() => {
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;
    let disposed = false;
    let ro: ResizeObserver | null = null;
    const still = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    (async () => {
      try {
        if (!hasWebGL2()) throw new Error('This browser has no WebGL 2, which the viewer needs.');
        const [{ createStrandViewer }, { createGroomEngine }] = await Promise.all([
          import('@/components/home/strand-three'),
          import('@/lib/groomlab-wasm'),
        ]);
        if (disposed) return;
        const viewer = createStrandViewer(canvas, { still, spin: 0.12, sway: 0.25, pitch: 0.12, fill: 0.92 });
        if (!viewer) throw new Error('The viewer could not start WebGL.');
        viewerRef.current = viewer;
        engineRef.current = createGroomEngine();
        const size = () => {
          const r = wrap.getBoundingClientRect();
          viewer.resize(r.width, r.height);
        };
        ro = new ResizeObserver(size);
        ro.observe(wrap);
        size();
        viewer.setActive(true);
        setReady(true);
        const url = sampleFromAddress() ?? sample;
        const response = await fetch(url);
        if (!response.ok) throw new Error(`Cannot fetch the sample groom (${response.status}).`);
        await load(await response.text(), undefined, url.split('/').pop() ?? 'sample.groom', [], response.url || url);
      } catch (e) {
        if (!disposed) {
          setError(e instanceof Error ? e.message : String(e));
          setStatus('');
        }
      }
    })();
    return () => {
      disposed = true;
      ro?.disconnect();
      viewerRef.current?.dispose();
      viewerRef.current = null;
      engineRef.current?.dispose();
      engineRef.current = null;
    };
  }, [sample, load]);

  const setValue = (c: GroomControl, v: number) => {
    setValues((old) => ({ ...old, [c.id]: v }));
    engineRef.current?.setParam(c.id, v).catch((e: unknown) => setError(String(e)));
    void evaluate();
  };

  const changeDensity = (v: number) => {
    densityRef.current = v;
    setDensity(v);
    void evaluate();
  };

  const download = async () => {
    const engine = engineRef.current;
    if (!engine) return;
    const bytes = await engine.saveGroom();
    const url = URL.createObjectURL(new Blob([bytes as BlobPart], { type: 'application/json' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = fileName.endsWith('.groom') ? fileName : fileName.replace(/\.json$/i, '') + '.groom';
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };

  /** Files given to the groom open now (its maps); re-evaluates when that changed what is missing. */
  const addFiles = async (picked: PickedFile[]) => {
    const engine = engineRef.current;
    if (!engine) return;
    try {
      const state = await engine.addFiles(await engineFiles(picked, null));
      setMissing(state.missing);
      await evaluate();
      if (state.failed.length) setError(state.failed.join('\n'));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  /**
   * Opens what was picked or dropped: a .groom (the shallowest; else a preset .json) with its .obj
   * and the files it reads (maps, beside it or in its folder tree). Without a groom, images go to
   * the groom open.
   */
  const openFiles = async (picked: PickedFile[]) => {
    const depth = (p: PickedFile) => p.path.split('/').length;
    const byDepth = [...picked].sort((a, b) => depth(a) - depth(b));
    const groom = byDepth.find((p) => /\.groom$/i.test(p.path)) ?? byDepth.find((p) => /\.json$/i.test(p.path));
    const mesh = byDepth.find((p) => /\.obj$/i.test(p.path));
    if (!groom) {
      if (version && picked.some((p) => READABLE.test(p.path))) return addFiles(picked);
      setError('Drop a .groom file (or a preset .json with an .obj mesh), with its maps or its folder.');
      return;
    }
    await load(await groom.file.text(), mesh ? await mesh.file.text() : undefined, groom.file.name, await engineFiles(picked, groom.path));
  };

  const onDrop = (e: DragEvent) => {
    e.preventDefault();
    setDragging(false);
    droppedFiles(e.dataTransfer)
      .then((picked) => (picked.length ? openFiles(picked) : undefined))
      .catch((err: unknown) => setError(err instanceof Error ? err.message : String(err)));
  };

  const groups: [string, GroomControl[]][] = [];
  for (const c of controls) {
    const g = groups.find(([name]) => name === c.group);
    if (g) g[1].push(c);
    else groups.push([c.group, [c]]);
  }

  return (
    <div
      className="relative grid lg:h-[calc(100dvh-4rem)] lg:grid-cols-[minmax(0,1fr)_360px]"
      onDragOver={(e) => {
        e.preventDefault();
        setDragging(true);
      }}
      onDragLeave={(e) => {
        if (e.currentTarget === e.target) setDragging(false);
      }}
      onDrop={onDrop}
    >
      <div ref={wrapRef} className="relative h-[62vh] min-h-[360px] bg-gl-paper lg:h-full">
        <canvas
          ref={canvasRef}
          aria-label="The groom, drawn in 3D: drag to turn it"
          role="img"
          style={{ touchAction: 'pan-y' }}
          className="absolute inset-0 block size-full cursor-grab"
        />
        {(status || !ready) && !error && (
          <p className="absolute inset-0 flex items-center justify-center font-mono text-[13px] text-gl-ink-2">{status}</p>
        )}
        <div className="pointer-events-none absolute top-3 left-3 flex flex-wrap gap-2 font-mono text-[11.5px] text-gl-ink-2">
          {stats && (
            <>
              <span className="rounded-md bg-[#16181d]/80 px-2 py-0.5 ring-1 ring-white/10">{stats.strands.toLocaleString('en-US')} strands</span>
              <span className="rounded-md bg-[#16181d]/80 px-2 py-0.5 ring-1 ring-white/10">{stats.points.toLocaleString('en-US')} points</span>
              <span className="rounded-md bg-[#16181d]/80 px-2 py-0.5 ring-1 ring-white/10" data-testid="eval-ms">
                evaluated in {stats.evaluateMs.toFixed(0)} ms
              </span>
            </>
          )}
        </div>
        {dragging && (
          <div className="pointer-events-none absolute inset-3 flex items-center justify-center rounded-xl border-2 border-dashed border-gl-blue bg-[#0b0c0f]/75 text-[15px] font-semibold text-gl-blue-soft">
            Drop a .groom (with its .obj and maps, or its folder) to open it
          </div>
        )}
        {fileName.startsWith('manny_') && (
          <p className="absolute bottom-3 left-3 max-w-[70%] text-[11.5px] leading-snug text-gl-ink-3">
            Manny head by Nick Rutlinh (GroomLab), CC BY 4.0
          </p>
        )}
      </div>

      <aside className="flex flex-col border-l border-white/[0.08] bg-gl-panel lg:min-h-0">
        <div className="border-b border-white/[0.08] px-4 py-3">
          <h1 className="text-[15px] font-semibold text-gl-ink">Groom viewer</h1>
          <p className="mt-0.5 text-[12.5px] leading-snug text-gl-ink-2">
            The GroomLab engine{version && ` ${version}`}, running in your browser (WebAssembly). Open a .groom, move its
            sliders, download the result.
          </p>
          <p className="mt-1 truncate font-mono text-[11.5px] text-gl-ink-3" title={fileName}>
            {fileName}
          </p>
        </div>

        <div className="flex-1 space-y-4 overflow-y-auto px-4 py-4">
          <section aria-label="Preview">
            <h2 className="mb-1.5 text-[11px] font-semibold tracking-wide text-gl-ink-3 uppercase">Preview</h2>
            <ValueSlider
              hue={category.Utility}
              label="Density"
              value={density}
              min={0.02}
              max={1}
              step={0.01}
              format={(v) => `${Math.round(v * 100)}%`}
              onChange={changeDensity}
            />
          </section>
          {groups.map(([group, items]) => (
            <section key={group} aria-label={group}>
              <h2 className="mb-1.5 text-[11px] font-semibold tracking-wide text-gl-ink-3 uppercase">{group}</h2>
              <div className="space-y-1.5">
                {items.map((c) => (
                  <ValueSlider
                    key={c.id}
                    hue={c.kind === 'control' ? category.Fields : category.Shape}
                    label={c.label}
                    title={c.description}
                    value={values[c.id] ?? c.value}
                    min={c.min}
                    max={c.max}
                    step={c.step}
                    format={(v) => v.toFixed(decimals(c.step))}
                    onChange={(v) => setValue(c, v)}
                    onReset={() => setValue(c, c.default)}
                  />
                ))}
              </div>
            </section>
          ))}
          {ready && !controls.length && !status && !error && (
            <p className="text-[12.5px] text-gl-ink-2">This groom has no sliders: no Control nodes or exposed params.</p>
          )}
          {missing.length > 0 && (
            <div role="status" data-testid="missing-files" className="rounded-lg bg-[#33290f] px-3 py-2 text-[12px] leading-snug text-[#ffd98a]">
              <p className="break-words">Missing: {missing.join(', ')}</p>
              <label className="mt-1 inline-flex cursor-pointer font-semibold underline underline-offset-2 has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-gl-blue">
                Add {missing.length === 1 ? 'it' : 'them'}…
                <input
                  type="file"
                  multiple
                  accept={IMAGE_ACCEPT}
                  className="sr-only"
                  onChange={(e) => {
                    if (e.target.files?.length) void addFiles(pickedOf(e.target.files));
                    e.target.value = '';
                  }}
                />
              </label>
            </div>
          )}
          {error && (
            <p role="alert" className="rounded-lg bg-[#3a1416] px-3 py-2 text-[12.5px] leading-snug text-[#ffb3b0]">
              {error}
            </p>
          )}
          {warnings.length > 0 && (
            <ul className="space-y-1 rounded-lg bg-[#33290f] px-3 py-2 text-[12px] leading-snug text-[#ffd98a]">
              {warnings.map((w) => (
                <li key={w}>{w}</li>
              ))}
            </ul>
          )}
        </div>

        <div className="flex gap-2 border-t border-white/[0.08] px-4 py-3">
          <label className="inline-flex h-9 flex-1 cursor-pointer items-center justify-center gap-1.5 rounded-[10px] bg-white/[0.07] px-3 text-[13px] font-semibold text-gl-ink ring-1 ring-white/10 ring-inset hover:bg-white/[0.11] has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-gl-blue">
            <FolderOpen aria-hidden="true" className="size-4" />
            Open
            <input
              type="file"
              multiple
              accept={`.groom,.json,.obj,${IMAGE_ACCEPT}`}
              className="sr-only"
              onChange={(e) => {
                if (e.target.files?.length) void openFiles(pickedOf(e.target.files));
                e.target.value = '';
              }}
            />
          </label>
          <label
            title="Open a folder: a .groom with its mesh and maps"
            className="inline-flex h-9 cursor-pointer items-center justify-center rounded-[10px] bg-white/[0.07] px-3 text-gl-ink ring-1 ring-white/10 ring-inset hover:bg-white/[0.11] has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-gl-blue"
          >
            <FolderTree aria-hidden="true" className="size-4" />
            <span className="sr-only">Open a folder</span>
            <input
              type="file"
              ref={(el) => el?.setAttribute('webkitdirectory', '')}
              className="sr-only"
              onChange={(e) => {
                if (e.target.files?.length) void openFiles(pickedOf(e.target.files));
                e.target.value = '';
              }}
            />
          </label>
          <button
            type="button"
            onClick={() => void download()}
            disabled={!stats}
            className="inline-flex h-9 flex-[1.6] items-center justify-center gap-1.5 rounded-[10px] whitespace-nowrap bg-gl-blue-strong px-3 text-[13px] font-semibold text-white hover:bg-[#1d5bcc] focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-gl-blue disabled:opacity-50"
          >
            <ArrowDownToLine aria-hidden="true" className="size-4" />
            Download .groom
          </button>
        </div>
      </aside>
    </div>
  );
}

/** The app's ValueSlider: a field that fills as it is dragged, its label and value inside. */
function ValueSlider({
  hue,
  label,
  title,
  value,
  min,
  max,
  step,
  onChange,
  onReset,
  format,
}: {
  hue: { hue: string; tint: string; ink: string };
  label: string;
  title?: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (v: number) => void;
  onReset?: () => void;
  format: (v: number) => string;
}) {
  const id = useId();
  const share = max > min ? Math.min(100, Math.max(0, ((value - min) / (max - min)) * 100)) : 0;
  return (
    <div
      title={title}
      onDoubleClick={onReset}
      className={cn(
        'relative h-7 overflow-hidden rounded-[7px] bg-[#1a1c21] font-mono text-[12px]',
        'has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-offset-2 has-[:focus-visible]:outline-gl-blue',
      )}
      style={{ '--fill': hue.hue } as CSSProperties}
    >
      <span aria-hidden="true" className="absolute inset-y-0 left-0" style={{ width: `${share}%`, background: hue.tint }} />
      <span aria-hidden="true" className="absolute inset-y-0 w-[2px]" style={{ left: `calc(${share}% - 1px)`, background: hue.ink }} />
      <label htmlFor={id} className="pointer-events-none absolute inset-y-0 left-2.5 flex items-center truncate text-gl-ink-2">
        {label}
      </label>
      <span aria-hidden="true" className="pointer-events-none absolute inset-y-0 right-2.5 flex items-center text-gl-ink tabular-nums">
        {format(value)}
      </span>
      <input
        id={id}
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        aria-valuetext={format(value)}
        onChange={(e) => onChange(Number(e.target.value))}
        className="absolute inset-0 size-full cursor-ew-resize appearance-none opacity-0"
      />
    </div>
  );
}
