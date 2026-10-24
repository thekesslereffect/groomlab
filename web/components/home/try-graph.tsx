// SPDX-License-Identifier: Apache-2.0
'use client';
import { ArrowRight, Eye, EyeOff, RotateCcw, Shuffle } from 'lucide-react';
import Link from 'next/link';
import { useId, useMemo, useState, type CSSProperties, type ReactNode } from 'react';
import type { GroomControl } from '@/lib/groomlab-wasm';
import { cn } from '@/lib/cn';
import {
  CURLS_FULL,
  CURLS_SEED,
  CURLS_SLIDERS,
  CURLS_URL,
  sliderValues,
  type EngineEdits,
  type EngineSlider,
  type EngineSource,
} from './engine-grooms';
import type { GroomStats } from './strand-3d';
import { category, port, type Category, type PortType } from './graph-theme';
import { StrandViewport } from './strand-viewport';

/*
 * "Try the node graph": the chain of a real groom (manny_curls.groom), node for node, grown by
 * the GroomLab engine in the browser. Each card is one of its nodes: its switch is the node's bypass,
 * its sliders the node's params with the engine's ranges and labels (the app's ValueSlider: the
 * field fills as it is dragged). The strands in the viewport are grown again on the spot, in the
 * engine's worker; the bare head shows until the engine has loaded.
 */

/** The nodes a card can bypass, by their names in the groom. */
type Bypassable = 'clump' | 'curl' | 'noise' | 'color';

interface State {
  /** Param values by slider id. */
  values: Record<string, number>;
  bypass: Record<Bypassable, boolean>;
  showGuides: boolean;
  /** Scatter's seed. */
  seed: number;
}

const initialState = (values: Record<string, number>): State => ({
  values,
  bypass: { clump: false, curl: false, noise: false, color: false },
  showGuides: false,
  seed: CURLS_SEED,
});

const SEED = 'param:scatter.seed';
const DENSITY = 'param:scatter.density';

const engineSource: EngineSource = {
  url: CURLS_URL,
  slot: 'try',
  fullStrands: CURLS_FULL,
  strands: 7000,
  densityParam: [DENSITY, CURLS_SLIDERS[0].value],
  maxStrands: 16000,
};

export function TryGraph() {
  // The sliders: the groom's exposed params, as the engine lists them once it has loaded.
  const [sliders, setSliders] = useState<EngineSlider[]>(CURLS_SLIDERS);
  const [s, setS] = useState<State>(() => initialState(sliderValues(CURLS_SLIDERS)));
  const [stats, setStats] = useState<GroomStats | null>(null);
  const strands = stats ? stats.strands.toLocaleString('en-US') : '7,000';
  const setValue = (id: string) => (v: number) => setS((prev) => ({ ...prev, values: { ...prev.values, [id]: v } }));
  const setOn = (node: Bypassable) => (on: boolean) => setS((prev) => ({ ...prev, bypass: { ...prev.bypass, [node]: !on } }));

  const engine = useMemo(
    () => ({
      source: engineSource,
      onControls: (controls: GroomControl[]) => {
        const next = CURLS_SLIDERS.map((fallback) => {
          const c = controls.find((x) => x.id === fallback.id);
          return c ? { id: c.id, label: c.label, min: c.min, max: c.max, step: c.step, value: c.value } : fallback;
        });
        setSliders(next);
      },
    }),
    [],
  );
  const edits = useMemo<EngineEdits>(() => ({ params: { ...s.values, [SEED]: s.seed }, bypass: { ...s.bypass } }), [s.values, s.seed, s.bypass]);

  const slider = (id: string, cat: Category) => {
    const spec = sliders.find((x) => x.id === id)!;
    return (
      <ValueSlider
        key={id}
        cat={cat}
        label={spec.label}
        value={s.values[id] ?? spec.value}
        min={spec.min}
        max={spec.max}
        step={spec.step}
        onChange={setValue(id)}
        format={(v) => v.toFixed(spec.step >= 0.1 ? 0 : 2)}
      />
    );
  };

  return (
    <div className="mt-14 grid gap-6 sm:mt-16 lg:grid-cols-12 lg:gap-10">
      {/* The viewport: first (and pinned under the header) on phones, beside the nodes on wide screens. */}
      <div className="sticky top-14 z-10 lg:order-2 lg:col-span-7 lg:self-start lg:top-20">
        <div className="bg-gl-paper lg:bg-transparent">
          <div className="flex h-8 items-center justify-end gap-3 font-mono text-[11.5px] text-gl-ink-3">
            <span className="flex items-center gap-1">
              <span className="mr-2 tabular-nums" aria-live="polite">
                {strands} strands
              </span>
              <IconButton label="New random roots" onClick={() => setS((prev) => ({ ...prev, seed: prev.seed + 1 }))}>
                <Shuffle className="size-3.5" strokeWidth={2.2} />
              </IconButton>
              <IconButton label="Reset the graph" onClick={() => setS(initialState(sliderValues(sliders)))}>
                <RotateCcw className="size-3.5" strokeWidth={2.2} />
              </IconButton>
            </span>
          </div>
          <div className="h-[min(46svh,360px)] lg:h-auto lg:aspect-[7/6]">
            <StrandViewport
              showGuides={s.showGuides}
              pitch={0.16}
              yaw={-0.6}
              framing={[[0, -0.3, 0], 1.85]}
              onStats={setStats}
              engine={engine}
              edits={edits}
              label="The groom the graph makes in 3D, grown again as the nodes change; drag to turn it"
            />
          </div>
          <p className="hidden justify-end pt-2 lg:flex">
            <Link
              href="/view?groom=manny_curls"
              className="inline-flex items-center gap-1.5 text-[13px] text-gl-ink-2 underline-offset-4 hover:text-gl-ink hover:underline"
            >
              Open it in the viewer
              <ArrowRight aria-hidden="true" className="size-3.5" strokeWidth={2.2} />
            </Link>
          </p>
        </div>
      </div>

      <ol className="lg:order-1 lg:col-span-5" aria-label="Nodes">
        <Node cat="Sources" name="Scalp" out="mesh">
          <Static label="mesh" value="manny_head" />
        </Node>
        <Node cat="Grow" name="Scatter" out="strands">
          {slider(DENSITY, 'Grow')}
        </Node>
        <Node
          cat="Grow"
          name="Interpolate"
          out="strands"
          action={
            <IconButton label="Show the guides" pressed={s.showGuides} onClick={() => setS((prev) => ({ ...prev, showGuides: !prev.showGuides }))}>
              {s.showGuides ? <Eye className="size-4" strokeWidth={2.2} /> : <EyeOff className="size-4" strokeWidth={2.2} />}
            </IconButton>
          }
        >
          <Static label="guides" value={String(stats ? stats.guides : 127)} />
        </Node>
        <Node cat="Shape" name="Transform" out="strands">
          {slider('param:length.scale', 'Shape')}
        </Node>
        <Node cat="Look" name="Clump" out="strands" on={!s.bypass.clump} onToggle={setOn('clump')}>
          {slider('param:clump.strength', 'Look')}
          {slider('param:clump.count', 'Look')}
        </Node>
        <Node cat="Look" name="Curl" out="strands" on={!s.bypass.curl} onToggle={setOn('curl')}>
          {slider('param:curl.radius', 'Look')}
          {slider('param:curl.frequency', 'Look')}
        </Node>
        <Node cat="Look" name="Noise" out="strands" on={!s.bypass.noise} onToggle={setOn('noise')}>
          {slider('param:noise.amplitude', 'Look')}
        </Node>
        <Node cat="Color" name="HairColor" out="strands" on={!s.bypass.color} onToggle={setOn('color')}>
          {slider('param:color.melanin', 'Color')}
          {slider('param:color.redness', 'Color')}
        </Node>
        <Node cat="Output" name="Output" last>
          <Static label="to the viewport" value={`${strands} strands`} />
        </Node>
      </ol>
      <p className="flex justify-center lg:hidden">
        <Link
          href="/view?groom=manny_curls"
          className="inline-flex items-center gap-1.5 text-[13px] text-gl-ink-2 underline-offset-4 hover:text-gl-ink hover:underline"
        >
          Open it in the viewer
          <ArrowRight aria-hidden="true" className="size-3.5" strokeWidth={2.2} />
        </Link>
      </p>
    </div>
  );
}

/** A node card, its input port on top and the wire to the next node below it. */
function Node({
  cat,
  name,
  out,
  on = true,
  onToggle,
  action,
  last,
  children,
}: {
  cat: Category;
  name: string;
  out?: PortType;
  on?: boolean;
  onToggle?: (v: boolean) => void;
  action?: ReactNode;
  last?: boolean;
  children: ReactNode;
}) {
  const c = category[cat];
  const dark = cat === 'Output';
  return (
    <li className="relative">
      <div
        className={cn(
          'try-node relative rounded-[12px] border bg-gl-panel transition-[opacity,border-color,box-shadow] duration-300',
          on ? 'border-white/[0.09] hover:border-white/[0.18] hover:shadow-[0_14px_30px_-18px_rgba(0,0,0,0.9)]' : 'border-dashed border-white/[0.14] opacity-60',
        )}
      >
        <div
          className="flex h-10 items-center gap-2.5 rounded-t-[11px] pr-1.5 pl-3 transition-colors duration-300"
          style={{ background: on ? c.hue : '#1a1c21', color: on ? c.on : undefined }}
        >
          <span className="text-[14px] font-semibold tracking-tight">{name}</span>
          {!on && <span className="font-mono text-[11px] text-gl-ink-3">bypassed</span>}
          <span className="ml-auto flex items-center">
            {action}
            {onToggle && <Switch label={`${name} node on`} checked={on} hue={dark ? c.hue : c.on} onChange={onToggle} />}
          </span>
        </div>
        <div className="space-y-2 px-3 py-3">{children}</div>
      </div>
      {!last && out && <VWire color={on ? port[out] : '#3a3d45'} />}
    </li>
  );
}

/** The wire between two stacked nodes, a pulse running down it. */
function VWire({ color }: { color: string }) {
  return (
    <div aria-hidden="true" className="relative ml-[17px] h-7 w-[10px]">
      <span className="absolute -top-[5px] left-0 size-[10px] rounded-full border-2 border-gl-paper" style={{ background: color }} />
      <span className="absolute inset-y-0 left-[4px] w-[2px] rounded-full" style={{ background: color }} />
      <span className="vwire-pulse absolute top-0 left-[3px] h-2 w-[4px] rounded-full" style={{ background: color }} />
      <span className="absolute -bottom-[5px] left-0 size-[10px] rounded-full border-2 border-gl-paper" style={{ background: color }} />
    </div>
  );
}

function Static({ label, value }: { label: string; value: string }) {
  return (
    <p className="flex h-7 items-center justify-between rounded-[7px] bg-[#1a1c21] px-2.5 font-mono text-[12px]">
      <span className="text-gl-ink-2">{label}</span>
      <span className="text-gl-ink">{value}</span>
    </p>
  );
}

/** The app's ValueSlider: a field that fills as it is dragged, its label and value inside. */
function ValueSlider({
  cat,
  label,
  value,
  min,
  max,
  step,
  onChange,
  format = (v) => v.toFixed(2),
}: {
  cat: Category;
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (v: number) => void;
  format?: (v: number) => string;
}) {
  const id = useId();
  const c = category[cat];
  const share = ((value - min) / (max - min)) * 100;
  return (
    <div
      className="value-slider relative h-7 overflow-hidden rounded-[7px] bg-[#1a1c21] font-mono text-[12px] has-[:focus-visible]:outline-2 has-[:focus-visible]:outline-offset-2 has-[:focus-visible]:outline-gl-blue"
      style={{ '--fill': c.hue } as CSSProperties}
    >
      <span aria-hidden="true" className="absolute inset-y-0 left-0" style={{ width: `${share}%`, background: c.tint }} />
      <span aria-hidden="true" className="absolute inset-y-0 w-[2px]" style={{ left: `calc(${share}% - 1px)`, background: c.ink }} />
      <label htmlFor={id} className="pointer-events-none absolute inset-y-0 left-2.5 flex items-center text-gl-ink-2">
        {label}
      </label>
      <span aria-hidden="true" className="pointer-events-none absolute inset-y-0 right-2.5 flex items-center tabular-nums text-gl-ink">
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

function Switch({ label, checked, hue, onChange }: { label: string; checked: boolean; hue: string; onChange: (v: boolean) => void }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={label}
      onClick={() => onChange(!checked)}
      className="grid h-8 w-11 place-items-center rounded-full focus-visible:outline-2 focus-visible:outline-gl-blue"
    >
      <span
        className="relative h-[18px] w-[30px] rounded-full transition-colors duration-200"
        style={{ background: checked ? hue : '#3a3d45' }}
      >
        <span
          className={cn(
            'absolute top-[2px] left-[2px] size-[14px] rounded-full bg-white shadow-sm transition-transform duration-300 ease-spring',
            checked && 'translate-x-[12px]',
          )}
        />
      </span>
    </button>
  );
}

function IconButton({
  label,
  pressed,
  onClick,
  children,
}: {
  label: string;
  pressed?: boolean;
  onClick: () => void;
  children: ReactNode;
}) {
  return (
    <button
      type="button"
      aria-label={label}
      aria-pressed={pressed}
      title={label}
      onClick={onClick}
      className={cn(
        'grid size-8 place-items-center rounded-full text-gl-ink-2 transition-colors hover:bg-white/[0.08] hover:text-gl-ink focus-visible:outline-2 focus-visible:outline-gl-blue',
        pressed && 'text-gl-blue-soft',
      )}
    >
      {children}
    </button>
  );
}
