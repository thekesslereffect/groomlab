// SPDX-License-Identifier: Apache-2.0
'use client';
import { useEffect, useState, useSyncExternalStore } from 'react';
import { TUNE_DEFAULTS, type TuneValues } from './strand-three';

/*
 * The viewers' shading knobs as sliders over the hero, shown with ?tune in the address: for tuning
 * the page's look (copy the values line into TUNE_DEFAULTS and the viewer's constants,
 * strand-three.ts). three.js loads lazily, so the knobs are applied through a dynamic import once
 * the viewer is up.
 */

const knobs: { key: keyof TuneValues; label: string; min: number; max: number; step: number }[] = [
  { key: 'highlightWidth', label: 'Highlight width', min: 0.2, max: 2, step: 0.01 },
  { key: 'specular', label: 'Specular', min: 0, max: 4, step: 0.01 },
  { key: 'key', label: 'Key light', min: 0, max: 5, step: 0.01 },
  { key: 'fill', label: 'Camera fill', min: 0, max: 3, step: 0.01 },
  { key: 'rim', label: 'Rim', min: 0, max: 3, step: 0.01 },
  { key: 'ambient', label: 'Ambient', min: 0, max: 3, step: 0.01 },
  { key: 'shadow', label: 'Self-shadow', min: 0, max: 4, step: 0.01 },
  { key: 'coverage', label: 'Coverage gain', min: 0.5, max: 10, step: 0.01 },
  { key: 'exposure', label: 'Exposure', min: 0.2, max: 3, step: 0.01 },
  { key: 'scalp', label: 'Scalp brightness', min: 0, max: 2, step: 0.01 },
  { key: 'transmission', label: 'Transmission (TT)', min: 0, max: 2, step: 0.01 },
  { key: 'scatter', label: 'Multiple scatter', min: 0, max: 2, step: 0.01 },
];

/** Whether the address asks for the panel (?tune); never on the server. */
const tuneAsked = () => new URLSearchParams(window.location.search).has('tune');
const noSubscribe = () => () => {};

export function TunePanel() {
  const on = useSyncExternalStore(noSubscribe, tuneAsked, () => false);
  const [v, setV] = useState<TuneValues>(TUNE_DEFAULTS);
  useEffect(() => {
    if (!on) return;
    let stop = false;
    // Applied now and again a moment later (the viewer may still be loading).
    const apply = () => import('./strand-three').then((m) => !stop && m.setTune(v));
    void apply();
    const id = window.setInterval(apply, 1000);
    return () => {
      stop = true;
      window.clearInterval(id);
    };
  }, [on, v]);
  if (!on) return null;
  return (
    <div className="fixed right-3 bottom-3 z-50 w-72 rounded-lg border border-white/15 bg-black/80 p-3 font-mono text-[11px] text-white backdrop-blur">
      <div className="mb-2 flex items-center justify-between">
        <span className="font-semibold">Hero shading (tune)</span>
        <button type="button" className="underline" onClick={() => setV(TUNE_DEFAULTS)}>
          reset
        </button>
      </div>
      {knobs.map((k) => (
        <label key={k.key} className="mb-1.5 block">
          <span className="flex justify-between">
            <span>{k.label}</span>
            <span>{v[k.key].toFixed(2)}</span>
          </span>
          <input
            type="range"
            className="w-full"
            min={k.min}
            max={k.max}
            step={k.step}
            value={v[k.key]}
            onChange={(e) => setV({ ...v, [k.key]: Number(e.target.value) })}
          />
        </label>
      ))}
      <textarea readOnly className="mt-1 h-16 w-full resize-none bg-white/10 p-1 text-[10px]" value={JSON.stringify(v)} />
    </div>
  );
}
