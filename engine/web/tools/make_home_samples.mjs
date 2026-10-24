// SPDX-License-Identifier: Apache-2.0
// Writes the home page's sample grooms with the WebAssembly module (build it first: docs/WEB.md), on
// the Manny head (hosts/maya/meshes/manny_head.obj, CC BY 4.0), each grown from a GroomLab preset
// with its guides baked from a small combing graph (as make_sample.mjs does):
//
//   web/public/samples/manny_curls.groom  the "Try the node graph" section: a big head of brown
//                                         curls (Curly, tuned) on exactly the section's chain, its
//                                         guides standing off the head, its "exposed" list the
//                                         section's sliders, in order.
//
// (The hero's ball of curls, webhero.groom, is groomed in Maya and exported as it is.)
//
// The page (web/components/home/engine-grooms.ts) mirrors the values printed at the end: keep the
// two in step when a value here changes.
//
//   node engine/web/tools/make_home_samples.mjs

import { readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const require = createRequire(import.meta.url);
const createGroomLab = require(resolve(root, 'web/public/wasm/groomlab.js'));

const mesh = readFileSync(resolve(root, 'hosts/maya/meshes/manny_head.obj'), 'utf8');
const curly = readFileSync(resolve(root, 'hosts/maya/presets/curly.json'), 'utf8');

// Where hair grows on Manny (cm, y up, facing +z), 0 to 1 at the root: above a hairline low at the
// nape and over the ears, higher at the temples and the brow (make_sample.mjs's). `drop` lowers it
// all, cm (a big head of curls grows from further down).
const hairline = (drop = 0) =>
  [
    '$front = smoothstep($z, -3, 7);',
    `$line = ${(-3.5 - drop).toFixed(2)} + 12.0 * $front + 2.5 * smoothstep(abs($x), 6, 9) * (1 - $front);`,
    'smoothstep($y, $line, $line + 1.5)',
  ].join(' ');

// The guides' roots are scattered at random (Scatter's randomness 1): evenly spread guides
// (randomness 0) stand the crown's curls up into a ball, where random ones let them fall and part.

/** The nodes and connections of a chain: `chain` names in order, strands wired one to the next. */
function chainGraph(nodes, chain, extra = []) {
  return {
    version: 1,
    output: 'output',
    nodes,
    connections: [...extra, ...chain.slice(1).map((to, i) => ({ from: chain[i], to, input: 'strands' }))],
  };
}

/**
 * Guides for a big head of curls: grown out along the scalp's normal, the front ones swept up and
 * back off the face, the sides a little down, then sagging a little under their weight and kept off
 * the head. Shorter at the face, longest at the crown and the back.
 */
function volumeGuides() {
  const length = 5.5;
  const spacing = 2.0;
  return chainGraph(
    [
      { name: 'scalp', type: 'Scalp' },
      {
        name: 'roots',
        type: 'Scatter',
        params: { density: { expression: hairline(0.8), min: 0, max: 1 / (spacing * spacing) }, length, randomness: 1, seed: 5 },
      },
      {
        name: 'lengths',
        type: 'Transform',
        params: { scaleRandom: 0.12, scale: { expression: '1 - 0.42 * smoothstep($nz, 0.15, 0.8) - 0.15 * smoothstep(-$ny, 0.1, 0.6)', min: 0, max: 1 } },
      },
      {
        name: 'sweep',
        type: 'Bend',
        params: {
          direction: { expression: 'normalize([$nx * 0.6, 0.9, -1.2 * smoothstep($nz, 0.1, 0.8) - 0.15])' },
          angle: 40,
          start: 0,
          end: 0.5,
          along: 'direction',
        },
      },
      { name: 'sag', type: 'Gravity', params: { strength: 0.35, stiffness: 0.6 } },
      { name: 'collide', type: 'Collide', params: { offset: 0.25, maxDepth: length, iterations: 4 } },
      { name: 'resample', type: 'Resample', params: { segments: 12 } },
      { name: 'output', type: 'Output' },
    ],
    ['roots', 'lengths', 'sweep', 'sag', 'collide', 'resample', 'output'],
    [
      { from: 'scalp', to: 'roots', input: 'scalp' },
      { from: 'scalp', to: 'collide', input: 'scalp' },
    ],
  );
}

const M = await createGroomLab();

/** Loads the Curly preset on Manny, bakes `guides`, lets `edit` reshape the graph, saves to `name`. */
function make(name, guides, edit) {
  const groom = new M.WebGroom();
  const check = (ok, what) => {
    if (!ok) {
      console.error(`${name}: ${what}: ${groom.error()}`);
      process.exit(1);
    }
  };
  check(groom.load(curly, mesh), 'load');
  check(groom.bakeGuides(JSON.stringify(guides)), 'guides');
  const graph = JSON.parse(groom.graphJson());
  const node = (n) => graph.nodes.find((x) => x.name === n);
  edit(graph, node);
  check(groom.setGraphJson(JSON.stringify(graph)), 'graph');
  check(groom.evaluate(1), 'evaluate');
  const stats = JSON.parse(groom.statsJson());
  if (stats.errors.length) console.error(stats.errors.join('\n'));
  const out = resolve(root, 'web/public/samples', name);
  writeFileSync(out, groom.saveGroom());
  const count = groom.strandCount();
  console.log(`${out}: ${count} strands at full density, ${stats.points} points (${(stats.points / count).toFixed(1)} a strand), ${groom.evaluateMs().toFixed(0)} ms`);
  for (const c of JSON.parse(groom.controlsJson())) console.log(`  ${c.id} "${c.label}" ${c.min}..${c.max} step ${c.step} = ${c.value}`);
  groom.delete();
}

// The Try section: Curly's ringlets, bigger and browner, on guides standing off the head, on the
// section's chain node for node: Scalp > Scatter > Interpolate > Transform ("length", scaling every
// strand from its root) > Clump > Curl > Noise > HairColor > Output (and the Guides node feeding
// Interpolate and Scatter). Scatter's density is a plain number (its slider), so the roots keep to
// the hair's part of the head by the guides instead: only within guideRadius of a guide's root. Its
// "exposed" list is the section's sliders, in order. (The hero, webhero.groom, is groomed in Maya.)
make('manny_curls.groom', volumeGuides(), (graph, node) => {
  node('scalp').params = { subdivision: '2' };
  Object.assign(node('scatter').params, { density: 120, seed: 7, guideRadius: 1.8 });
  node('clump').params = { clumpBy: 'count', count: 300, strength: 0.8 };
  Object.assign(node('curl').params, { radius: 0.42, frequency: 0.42, randomness: 0.3, tolerance: 0.08 });
  const noise = node('frizzNoise');
  noise.name = 'noise';
  Object.assign(noise.params, { fraction: 0.15, amplitude: 0.12 });
  Object.assign(node('color').params, { melanin: 0.5, redness: 0.42, melaninRandom: 0.1 });
  // No Width or Resample (Curl adds the points a turn needs); the length scale between Interpolate
  // and Clump.
  graph.nodes = graph.nodes.filter((n) => n.name !== 'width' && n.name !== 'resample');
  graph.nodes.push({ name: 'length', type: 'Transform', params: { scale: 1.8 }, ui: { x: 720, y: 0 } });
  for (const c of graph.connections) {
    if (c.from === 'frizzNoise') c.from = 'noise';
    if (c.to === 'frizzNoise') c.to = 'noise';
  }
  graph.connections = graph.connections.filter(
    (c) => !(c.from === 'interpolate' && c.to === 'clump') && !['width', 'resample'].includes(c.from) && !['width', 'resample'].includes(c.to),
  );
  graph.connections.push(
    { from: 'interpolate', to: 'length', input: 'strands' },
    { from: 'length', to: 'clump', input: 'strands' },
    { from: 'color', to: 'output', input: 'strands' },
    { from: 'guides', to: 'scatter', input: 'guides' },
  );
  graph.exposed = [
    { node: 'scatter', param: 'density', name: 'density' },
    { node: 'length', param: 'scale', name: 'length' },
    { node: 'clump', param: 'strength', name: 'strength' },
    { node: 'clump', param: 'count', name: 'count' },
    { node: 'curl', param: 'radius', name: 'radius' },
    { node: 'curl', param: 'frequency', name: 'frequency' },
    { node: 'noise', param: 'amplitude', name: 'amplitude' },
    { node: 'color', param: 'melanin', name: 'melanin' },
    { node: 'color', param: 'redness', name: 'redness' },
  ];
});
