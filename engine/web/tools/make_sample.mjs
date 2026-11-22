// SPDX-License-Identifier: Apache-2.0
// Writes the website's sample groom (web/public/samples/manny_hair.groom) with the WebAssembly
// module (build it first: docs/WEB.md): the Hair basics preset on the Manny head
// (engine/assets/meshes/manny_head.obj, CC BY 4.0), its strands kept to the hair's part of the head by
// an expression, its guides baked from a small combing graph (as tests/hosts/maya/gui_presets.py
// grooms the preset sheets' heads), a Control node, "frizz", scaling the noise, and two painted
// maps, so the page exercises both kinds: "cut", a vertex map (inside the .groom: shorter on the
// head's right, an asymmetric cut through a Length node), and "highlights", a texture map (an image
// file at the scalp's UVs, where the standalone app keeps painted texture maps:
// masks/manny_hair/highlights.png beside the groom; drawn here, procedurally: streaks from the crown
// and two face-framing pieces) giving HairColor's greys, light streaks in the brown.
//
//   node engine/web/tools/make_sample.mjs

import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { grayPng } from './png.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const require = createRequire(import.meta.url);
const createGroomLab = require(resolve(root, 'web/public/wasm/groomlab.js'));

// Where hair grows on Manny (cm, y up, facing +z): above a hairline low at the nape and over the
// ears, higher at the temples and the brow. 0 to 1 at the root.
const HAIR = [
  '$front = smoothstep($z, -3, 7);',
  '$line = -3.5 + 12.0 * $front + 2.5 * smoothstep(abs($x), 6, 9) * (1 - $front);',
  'smoothstep($y, $line, $line + 1.5)',
].join(' ');

const LENGTH = 24; // the back's guide length, cm
const SPACING = 2.4; // between guides, cm

// Guides as an artist grooms a shoulder-length style: longer at the back, combed away from a center
// part and back off the face, a little lift, then hanging under gravity and kept off the head.
function combGraph() {
  const side = '(smoothstep($nx, -0.12, 0.12) * 2 - 1)';
  const nodes = [
    { name: 'scalp', type: 'Scalp' },
    {
      name: 'roots',
      type: 'Scatter',
      params: { density: { expression: HAIR, min: 0, max: 1 / (SPACING * SPACING) }, length: LENGTH, randomness: 0, seed: 3 },
    },
    {
      name: 'lengths',
      type: 'Transform',
      params: { scaleRandom: 0.06, scale: { expression: '1 - 0.72 * smoothstep($nz, -0.2, 0.75)', min: 0, max: 1 } },
    },
    {
      name: 'part',
      type: 'Bend',
      params: {
        direction: {
          expression: `normalize([${side} * (1 + 2 * smoothstep($nz, 0.3, 0.9)), -0.5 + 1.2 * smoothstep($nz, 0.35, 0.9), -0.6 - 1.4 * smoothstep($nz, 0.3, 0.85)])`,
        },
        angle: 60,
        start: 0,
        end: 0.2,
        along: 'direction',
      },
    },
    { name: 'lift', type: 'Bend', params: { angle: 8, end: 0 } },
    { name: 'hang', type: 'Gravity', params: { strength: 1.3, stiffness: 0.2 } },
    { name: 'collide', type: 'Collide', params: { offset: LENGTH * 0.012, maxDepth: LENGTH, iterations: 4 } },
    { name: 'resample', type: 'Resample', params: { segments: 12 } },
    { name: 'output', type: 'Output' },
  ];
  const chain = ['roots', 'lengths', 'part', 'lift', 'hang', 'collide', 'resample', 'output'];
  const connections = [
    { from: 'scalp', to: 'roots', input: 'scalp' },
    { from: 'scalp', to: 'collide', input: 'scalp' },
    ...chain.slice(1).map((to, i) => ({ from: chain[i], to, input: 'strands' })),
  ];
  return { version: 1, output: 'output', nodes, connections };
}

const smoothstep = (x, a, b) => {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
};

// The highlights at Manny's UVs (u around the head, the back at 0 and 1, the face at 0.5; v up the
// head, the crown at the top): streaks of uneven strength running down from the crown, and two
// pieces framing the face either side of the part. White: a highlight.
function highlightsPng(size) {
  const pixels = new Uint8Array(size * size);
  const bands = 13;
  const strength = (k) => 0.35 + 0.65 * ((Math.sin(k * 12.9898 + 4.1) * 43758.5453) % 1 + 1) % 1;
  for (let y = 0; y < size; y++) {
    const v = 1 - (y + 0.5) / size;
    for (let x = 0; x < size; x++) {
      const u = (x + 0.5) / size;
      const t = u * bands + 0.18 * Math.sin(v * 7.3);
      const k = ((Math.floor(t) % bands) + bands) % bands;
      const streak = Math.pow(0.5 + 0.5 * Math.cos(2 * Math.PI * (t - Math.floor(t) - 0.5)), 3) * strength(k);
      const face = Math.exp(-(((Math.abs(u - 0.5) - 0.055) / 0.018) ** 2)) * smoothstep(v, 0.45, 0.7);
      pixels[y * size + x] = Math.round(255 * Math.min(1, Math.max(streak, face)));
    }
  }
  return grayPng(size, size, pixels);
}

// The cut: the share of each strand's length kept, by scalp vertex: all of it on the head's left
// (+x), down to 40% on its right, shortest toward the face.
function cutValue(x, z) {
  return 1 - 0.6 * (1 - smoothstep(x, -7, 5)) * (0.6 + 0.4 * smoothstep(z, -4, 6));
}

const M = await createGroomLab();
const groom = new M.WebGroom();
const preset = readFileSync(resolve(root, 'engine/assets/presets/hair_basics.json'), 'utf8');
const mesh = readFileSync(resolve(root, 'engine/assets/meshes/manny_head.obj'), 'utf8');
const check = (ok, what) => {
  if (!ok) {
    console.error(`${what}: ${groom.error()}`);
    process.exit(1);
  }
};
check(groom.load(preset, mesh), 'load');
check(groom.bakeGuides(JSON.stringify(combGraph())), 'guides');

// The preset's strands on the hair's part of the head only, kept off it like the guides, and a
// Control node, "frizz", scaling Noise's amplitude (plugged into its exposed amplitude).
const graph = JSON.parse(groom.graphJson());
const node = (name) => graph.nodes.find((n) => n.name === name);
const scatter = node('scatter');
scatter.params.density = { expression: HAIR, min: 0, max: scatter.params.density * 1.6 };
const noise = node('noise');
noise.paramInputs = ['amplitude'];
noise.params.amplitude *= 2;
graph.nodes.push({ name: 'frizz', type: 'Control', params: { name: 'frizz', default: 1, min: 0, max: 4 } });
graph.connections.push({ from: 'frizz', to: 'noise', input: 'amplitude' });
// The maps, read by Map nodes plugged into the params they drive (a map multiplies the param):
// the cut into a Length node's keep after the interpolation, the highlights into HairColor's greys.
graph.nodes.push(
  { name: 'cutMap', type: 'Map', params: { name: 'cut' } },
  { name: 'cut', type: 'Length', params: { keep: 1 }, paramInputs: ['keep'] },
  { name: 'highlightsMap', type: 'Map', params: { name: 'highlights' } },
);
graph.connections = graph.connections.filter((c) => !(c.from === 'interpolate' && c.to === 'clump'));
graph.connections.push(
  { from: 'interpolate', to: 'cut', input: 'strands' },
  { from: 'cut', to: 'clump', input: 'strands' },
  { from: 'cutMap', to: 'cut', input: 'keep' },
  { from: 'highlightsMap', to: 'color', input: 'greys' },
);
const color = node('color');
color.params.greys = 0.9;
color.paramInputs = [...(color.paramInputs ?? []), 'greys'];
check(groom.setGraphJson(JSON.stringify(graph)), 'graph');

// The maps into the file: the vertex map as a color set of the scalp (gray: r = g = b, a = 1, per
// triangle corner), both in the groom's map catalog; the texture beside the groom.
const texturePath = 'masks/manny_hair/highlights.png';
const file = JSON.parse(groom.saveGroom());
const { points, triangles } = file.scalp;
const cut = [];
for (const v of triangles) {
  const k = cutValue(points[v * 3], points[v * 3 + 2]);
  cut.push(k, k, k, 1);
}
file.scalp.colorSets = { ...(file.scalp.colorSets ?? {}), cut };
file.maps = { cut: { kind: 'vertices', texture: '' }, highlights: { kind: 'texture', texture: texturePath } };
const png = highlightsPng(256);
const out = resolve(root, 'web/public/samples/manny_hair.groom');
mkdirSync(dirname(resolve(dirname(out), texturePath)), { recursive: true });
writeFileSync(resolve(dirname(out), texturePath), png);
check(groom.load(JSON.stringify(file), ''), 'reload');
check(groom.addFile(texturePath, png), 'texture');
const missing = JSON.parse(groom.missingFilesJson());
if (missing.length) {
  console.error(`missing: ${missing.join(', ')}`);
  process.exit(1);
}

check(groom.evaluate(1), 'evaluate');
const stats = JSON.parse(groom.statsJson());
if (stats.errors.length) console.error(stats.errors.join('\n'));
writeFileSync(out, groom.saveGroom());
console.log(`${out}: ${groom.strandCount()} strands at full density, ${stats.points} points, ${groom.evaluateMs().toFixed(0)} ms`);
console.log(JSON.parse(groom.controlsJson()).map((c) => `${c.id} ${c.min}..${c.max} = ${c.value}`).join('\n'));
groom.delete();
