// SPDX-License-Identifier: Apache-2.0
/*
 * The GroomLab engine in a worker: groomlab.js and groomlab.wasm (engine/web, built with the wasm
 * preset: docs/WEB.md) evaluating a groom off the page's thread. web/lib/groomlab-wasm is its client.
 * Messages: {id, op, args} in, {id, ok, result} or {id, ok: false, error} out; buffers are copied
 * out of the module's memory and transferred. One worker holds several grooms at once, each in a
 * slot (args.slot, "main" when left out): the home page evaluates its three viewers' grooms here.
 */
/* global importScripts, createGroomLab */
'use strict';

let modulePromise = null;
/** slot name -> WebGroom */
const grooms = new Map();

function engine() {
  if (!modulePromise) {
    importScripts('groomlab.js');
    modulePromise = createGroomLab();
  }
  return modulePromise;
}

function need(slot) {
  const g = grooms.get(slot || 'main');
  if (!g) throw new Error('no groom loaded');
  return g;
}

function scalpOf(g) {
  return {
    positions: g.scalpPositions().slice(),
    normals: g.scalpNormals().slice(),
    indices: g.scalpIndices().slice(),
  };
}

// Files into the module's memory file system (WebGroom.addFile: at the groom's folder, matched to
// the missing references by name). [[path, bytes]...] in; the paths that failed, with why, out.
function addFiles(g, files) {
  const failed = [];
  for (const [path, bytes] of files || []) {
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (!g.addFile(path, data)) failed.push(path + ': ' + g.error());
  }
  return failed;
}

function filesOf(g) {
  return { files: JSON.parse(g.filesJson()), missing: JSON.parse(g.missingFilesJson()) };
}

const ops = {
  async load({ slot, groom: text, mesh, files }) {
    const m0 = performance.now();
    const M = await engine();
    const moduleMs = performance.now() - m0;
    const next = new M.WebGroom();
    const t0 = performance.now();
    if (!next.load(text, mesh || '')) {
      const error = next.error();
      next.delete();
      throw new Error(error);
    }
    const key = slot || 'main';
    const old = grooms.get(key);
    if (old) {
      old.clearFiles();
      old.delete();
    }
    const groom = next;
    grooms.set(key, groom);
    const failed = addFiles(groom, files);
    return {
      version: M.version(),
      loadMs: performance.now() - t0,
      // How long this load waited for the module (the first: fetching and compiling it).
      moduleMs,
      controls: JSON.parse(groom.controlsJson()),
      scalp: scalpOf(groom),
      failed,
      ...filesOf(groom),
    };
  },
  unload({ slot }) {
    const g = grooms.get(slot || 'main');
    if (g) {
      g.clearFiles();
      g.delete();
      grooms.delete(slot || 'main');
    }
    return null;
  },
  addFiles({ slot, files }) {
    const g = need(slot);
    const failed = addFiles(g, files);
    return { failed, ...filesOf(g) };
  },
  files({ slot }) {
    return filesOf(need(slot));
  },
  controls({ slot }) {
    return JSON.parse(need(slot).controlsJson());
  },
  set({ slot, values }) {
    const g = need(slot);
    const failed = [];
    for (const [id, value] of values) if (!g.setParam(id, value)) failed.push(id + (g.error() ? ': ' + g.error() : ''));
    return { failed };
  },
  // Nodes bypassed or not ([[node, bypassed]...]): the graph's own "bypass" flag, set with
  // setGraphJson, which keeps every node before them.
  bypass({ slot, nodes }) {
    const g = need(slot);
    const graph = JSON.parse(g.graphJson());
    const failed = [];
    for (const [name, bypassed] of nodes) {
      const node = (graph.nodes || []).find((n) => n && n.name === name);
      if (!node) failed.push(name + ': no such node');
      else if (bypassed) node.bypass = true;
      else delete node.bypass;
    }
    if (!g.setGraphJson(JSON.stringify(graph))) throw new Error(g.error());
    return { failed };
  },
  evaluate({ slot, density }) {
    const g = need(slot);
    const t0 = performance.now();
    const ok = g.evaluate(density);
    const stats = JSON.parse(g.statsJson());
    if (!ok) throw new Error(g.error());
    const guideOffsets = g.guideOffsets().slice();
    return {
      count: g.strandCount(),
      offsets: g.offsets().slice(),
      positions: g.positions().slice(),
      colors: g.colors().slice(),
      widths: g.widths().slice(),
      guides: guideOffsets.length > 1 ? { offsets: guideOffsets, positions: g.guidePositions().slice() } : null,
      evaluateMs: g.evaluateMs(),
      totalMs: performance.now() - t0,
      errors: stats.errors,
      nodes: stats.nodes,
      look: stats.look,
    };
  },
  scalp({ slot }) {
    return scalpOf(need(slot));
  },
  save({ slot }) {
    return need(slot).saveGroom();
  },
};

function transferables(result) {
  const out = [];
  const walk = (v) => {
    if (!v || typeof v !== 'object') return;
    if (ArrayBuffer.isView(v)) out.push(v.buffer);
    else if (!Array.isArray(v)) Object.values(v).forEach(walk);
  };
  walk(result);
  return out;
}

self.onmessage = async (e) => {
  const { id, op, args } = e.data;
  try {
    if (!ops[op]) throw new Error('unknown op ' + op);
    const result = await ops[op](args || {});
    self.postMessage({ id, ok: true, result }, transferables(result));
  } catch (err) {
    self.postMessage({ id, ok: false, error: String((err && err.message) || err) });
  }
};
