// SPDX-License-Identifier: Apache-2.0
/*
 * The home page's real grooms, evaluated in the browser by the engine built to WebAssembly
 * (home-engine.ts, loaded lazily): the hero's ball of curls (webhero.groom, groomed in Maya and
 * exported as it is) and the curly cut on the Manny head that the Try section plays with (made by
 * the engine from a preset: engine/web/tools/make_home_samples.mjs, CC BY 4.0). Small and dependency free, so the page can
 * draw its controls before the engine has loaded: the values here mirror what the generator printed
 * (the engine's own ranges, labels and values replace them once it answers).
 */

/** A slider: one of the groom's exposed params, as the engine lists it (GroomControl's fields). */
export interface EngineSlider {
  /** "param:<node>.<param>": what setParam takes. */
  id: string;
  label: string;
  min: number;
  max: number;
  step: number;
  value: number;
}

/** What a viewer asks of the engine: a groom, and how many strands it shows. */
export interface EngineSource {
  /** The .groom's URL. */
  url: string;
  /** Its slot in the page's one engine (one groom per slot). */
  slot: string;
  /** The groom's strands at full density (the generator's count): the preview density is a share of it. */
  fullStrands: number;
  /** About how many strands the viewer draws (desktop; phones take ~half). */
  strands: number;
  /** A param scaling the strand count (Scatter's density) and its value in the file: the count follows it, up to `maxStrands`. */
  densityParam?: [string, number];
  /** The most strands the viewer draws whatever that param says. */
  maxStrands?: number;
  /**
   * The groom's space to the viewers' (groom space: the Manny head about 2 across, centered): its
   * center and scale. Default: the Manny head's, in centimetres.
   */
  space?: { center: [number, number, number]; scale: number };
  /** Draws the bare Manny head until the engine's scalp comes (default); false for a groom on another mesh. */
  bareHead?: boolean;
}

/** What the page sets on a groom: param values ("param:<node>.<param>") and bypassed nodes (by name). */
export interface EngineEdits {
  params?: Record<string, number>;
  bypass?: Record<string, boolean>;
}

/** The hero: a ball of brown curls on a 5 cm sphere (webhero.groom), scaled to about the head's size. */
export const HERO_GROOM: EngineSource = {
  url: '/samples/webhero.groom',
  slot: 'hero',
  fullStrands: 46595,
  strands: 22000,
  space: { center: [0, 0, 0], scale: 0.17 },
  bareHead: false,
};
/** The Try section's curly cut on the Manny head, on its chain node for node. */
export const CURLS_URL = '/samples/manny_curls.groom';
export const CURLS_FULL = 79040;
/** Scatter's seed in the file (a new seed: new roots). */
export const CURLS_SEED = 7;

/** The groom's exposed params, in the Try section's order. */
export const CURLS_SLIDERS: EngineSlider[] = [
  { id: 'param:scatter.density', label: 'Density', min: 0, max: 2000, step: 0.1, value: 120 },
  { id: 'param:length.scale', label: 'Length', min: 0, max: 2, step: 0.001, value: 1.8 },
  { id: 'param:clump.strength', label: 'Strength', min: -1, max: 1, step: 0.001, value: 0.8 },
  { id: 'param:clump.count', label: 'Count', min: 0, max: 1000, step: 1, value: 300 },
  { id: 'param:curl.radius', label: 'Radius', min: 0, max: 2, step: 0.001, value: 0.42 },
  { id: 'param:curl.frequency', label: 'Frequency', min: 0, max: 2, step: 0.001, value: 0.42 },
  { id: 'param:noise.amplitude', label: 'Amplitude', min: 0, max: 1, step: 0.001, value: 0.12 },
  { id: 'param:color.melanin', label: 'Melanin', min: 0, max: 1, step: 0.001, value: 0.5 },
  { id: 'param:color.redness', label: 'Redness', min: 0, max: 1, step: 0.001, value: 0.42 },
];

export const sliderValues = (sliders: EngineSlider[]) => Object.fromEntries(sliders.map((s) => [s.id, s.value]));
