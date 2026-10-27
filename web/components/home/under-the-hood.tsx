// SPDX-License-Identifier: Apache-2.0
import type { CSSProperties, ReactNode } from 'react';
import { Reveal } from '@/components/reveal';
import { TextLink } from '@/components/text-link';
import { cn } from '@/lib/cn';
import { category, type Category } from './graph-theme';
import { SectionHead } from './section-node';

/* Every node in the graph, by category (see content/docs/node-graph.mdx). */
const categories: { name: string; cat: Category; nodes: string[] }[] = [
  { name: 'Sources', cat: 'Sources', nodes: ['Scalp', 'Guides', 'ColliderMesh', 'ImportCurves', 'Reference'] },
  { name: 'Grow', cat: 'Grow', nodes: ['Scatter', 'Interpolate', 'Cull'] },
  {
    name: 'Shape',
    cat: 'Shape',
    nodes: ['Length', 'Width', 'Resample', 'Sculpt', 'Direction', 'Bend', 'Transform', 'Flow', 'Part', 'Smooth', 'BlendNeighbors', 'Gravity', 'Displace', 'Mirror', 'Collide', 'SelfCollide', 'MeshCut', 'RestoreLength'],
  },
  { name: 'Look', cat: 'Look', nodes: ['Clump', 'Wet', 'Twist', 'Braid', 'Noise', 'Curl', 'Flyaway', 'Static', 'Tangle', 'Instance', 'Scales'] },
  { name: 'Motion', cat: 'Motion', nodes: ['Wind', 'Jiggle', 'Simulate'] },
  { name: 'Feathers', cat: 'Feathers', nodes: ['Propagate', 'Plume', 'Split'] },
  { name: 'Color', cat: 'Color', nodes: ['HairColor', 'Bands', 'FeatherColor', 'RenderData'] },
  { name: 'Convert', cat: 'Convert', nodes: ['ToGuides', 'ToStrands', 'StrandMesh'] },
  { name: 'Fields & Curves', cat: 'Fields', nodes: ['Map', 'Expression', 'Control', 'FieldNoise', 'FieldRemap', 'FieldCombine', 'Curve'] },
  { name: 'Utility', cat: 'Utility', nodes: ['Group', 'Blend', 'Attribute', 'Merge', 'Switch', 'Output'] },
  { name: 'Games', cat: 'Games', nodes: ['Cards'] },
];

const nodes = categories.flatMap((c) => c.nodes.map((n) => ({ name: n, cat: c.cat })));
const half = Math.ceil(nodes.length / 2);
const rows = [nodes.slice(0, half), nodes.slice(half)];

const specs: { title: string; href: string }[] = [
  { title: 'A procedural for the farm', href: '/docs/rendering' },
  { title: 'The same groom on every machine', href: '/docs/groom-format' },
  { title: 'Maps and keys on any parameter', href: '/docs/painting-maps' },
  { title: 'Python API and a headless CLI', href: '/docs/python-api' },
  { title: 'An MCP server for AI agents', href: '/docs/ai-agents' },
];

/* From the Python API docs (content/docs/python-api.mdx). */
const code = `import groomlab
from groomlab import graph

groom = groomlab.create_groom("head")

model = graph.GraphModel.from_groom(groom)
model.add_node("Curl", name="curl")
model.insert_between("noise", "color", "curl")
model.set_param("curl", "radius", 0.08)
model.save(groom)   # undoable

groomlab.export_alembic(groom, "C:/cache/groom.abc",
                        start=1, end=48)`;

/* The example session in the AI agents docs (content/docs/ai-agents.mdx), shortened. */
const session: { who: 'you' | 'tool'; text: string }[] = [
  { who: 'you', text: 'Make tube-lock anime hair on Nika and show me.' },
  { who: 'tool', text: 'new_groom  {"mesh": "nika", "preset": "tube_locks"}' },
  { who: 'tool', text: 'add_guides  {"length": 18, "droop": 0.8}' },
  { who: 'tool', text: 'render_preview  {"views": ["front", "side"]}' },
  { who: 'tool', text: 'save_groom  {"path": "anime.groom"}' },
];

export function UnderTheHood() {
  return (
    <section aria-labelledby="hood-title" className="section-y overflow-x-clip">
      <div className="wrap">
        <SectionHead
          id="hood-title"
          cat="Utility"
          index="05"
          tag="Under the hood"
          title="Built for groom artists."
          muted="Down to the strand."
          lede={<p>Over sixty nodes, a file format that opens anywhere, and every action scriptable.</p>}
        />
      </div>

      <Reveal className="mt-14 space-y-3 sm:mt-16">
        <p className="sr-only">GroomLab&apos;s nodes: {nodes.map((n) => n.name).join(', ')}.</p>
        {rows.map((row, i) => (
          <NodeRow key={i} nodes={row} reverse={i === 1} duration={i === 0 ? 80 : 74} />
        ))}
      </Reveal>
      <div className="wrap mt-6">
        <TextLink href="/docs/node-graph">Every node, explained</TextLink>
      </div>

      <div className="wrap mt-16 grid grid-cols-[minmax(0,1fr)] gap-5 sm:mt-20 lg:grid-cols-12">
        <Reveal className="lg:col-span-4">
          <ul className="grid gap-2.5">
            {specs.map((s) => (
              <li key={s.title}>
                <a
                  href={s.href}
                  className="group flex items-center gap-3 border-b border-white/[0.08] py-3.5 pr-1 text-[15px] font-semibold tracking-tight text-gl-ink transition-colors hover:text-gl-blue-soft focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-gl-blue"
                >
                  {s.title}
                  <svg aria-hidden="true" viewBox="0 0 16 16" className="ml-auto size-3.5 text-gl-ink-3 transition-transform duration-500 ease-spring group-hover:translate-x-1 group-hover:text-gl-ink" fill="none">
                    <path d="M6 3.5 10.5 8 6 12.5" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
                  </svg>
                </a>
              </li>
            ))}
          </ul>
        </Reveal>
        <Reveal delay={90} className="lg:col-span-5">
          <Card name="make_groom.py" cat="Utility">
            <pre className="overflow-x-auto px-4 py-4 font-mono text-[12px] leading-[1.75] sm:text-[12.5px]">
              <code>
                {code.split('\n').map((line, i) => (
                  <span key={i} className="block min-h-[1lh]">
                    {highlight(line)}
                  </span>
                ))}
              </code>
            </pre>
          </Card>
        </Reveal>
        <Reveal delay={180} className="lg:col-span-3">
          <Card name="groomlab-cli mcp" cat="Color">
            <ol className="space-y-2 px-4 py-4 font-mono text-[11.5px] leading-snug">
              {session.map((m, i) => (
                <li
                  key={i}
                  className={cn('mcp-line rounded-[8px] px-2.5 py-2', m.who === 'you' ? 'bg-gl-blue-strong font-sans text-[13px] text-white' : 'bg-[#1a1c21] text-gl-ink-2')}
                  style={{ '--d': `${0.3 + i * 0.35}s` } as CSSProperties}
                >
                  {m.who === 'tool' ? (
                    <>
                      <span className="font-medium text-[#ff9fcb]">{m.text.split('  ')[0]}</span> {m.text.split('  ')[1]}
                    </>
                  ) : (
                    m.text
                  )}
                </li>
              ))}
            </ol>
          </Card>
        </Reveal>
      </div>
      <div className="wrap mt-6 flex flex-col gap-3 sm:flex-row sm:gap-8">
        <TextLink href="/docs/python-api">The Python API</TextLink>
        <TextLink href="/docs/ai-agents">Use GroomLab from AI agents</TextLink>
      </div>
    </section>
  );
}

/** A node-like card with a header and a body. */
function Card({ name, cat, children }: { name: string; cat: Category; children: ReactNode }) {
  const c = category[cat];
  return (
    <figure className="h-full overflow-hidden rounded-[16px] bg-gl-panel text-gl-ink">
      <figcaption className="flex h-10 items-center gap-2.5 px-3.5 font-mono text-[12px]" style={{ background: c.hue, color: c.on }}>
        {name}
      </figcaption>
      {children}
    </figure>
  );
}

/**
 * A row of node chips on a wire, drifting sideways. The chips are drawn twice so the loop is
 * seamless; for readers who ask for reduced motion the row stands still and wraps instead.
 */
function NodeRow({ nodes: row, reverse, duration }: { nodes: { name: string; cat: Category }[]; reverse?: boolean; duration: number }) {
  const chips = (copy: number) =>
    row.map((n) => (
      <li key={`${copy}-${n.name}`} className={cn(copy > 0 && 'marquee-copy')}>
        <span className="relative flex h-9 items-center gap-2 rounded-[9px] border border-white/[0.1] bg-gl-panel pr-3.5 pl-1.5 font-mono text-[12.5px] font-medium text-gl-ink">
          <span aria-hidden="true" className="h-6 w-1.5 rounded-full" style={{ background: category[n.cat].hue }} />
          {n.name}
        </span>
      </li>
    ));
  return (
    <div aria-hidden="true" className="marquee-row relative">
      <span className="marquee-wire absolute inset-x-0 top-1/2 h-px bg-[#24272e]" />
      <ul className={cn('marquee relative flex', reverse && 'marquee-reverse')} style={{ '--marquee-dur': `${duration}s` } as CSSProperties}>
        {chips(0)}
        {chips(1)}
      </ul>
    </div>
  );
}

const TOKEN = /(#.*$)|("[^"]*")|\b(import|from)\b|\b(\d+(?:\.\d+)?)\b|(\w+)(?=\()/g;

/** Syntax colors for the dark card, light enough for AA on it. */
function highlight(line: string): ReactNode[] {
  const out: ReactNode[] = [];
  let last = 0;
  for (const m of line.matchAll(TOKEN)) {
    if (m.index > last) out.push(line.slice(last, m.index));
    const color = m[1] ? 'text-gl-ink-3' : m[2] ? 'text-[#7ee2a8]' : m[3] ? 'text-[#c3a6ff]' : m[4] ? 'text-[#ffc96b]' : 'text-[#82b6ff]';
    out.push(
      <span key={m.index} className={color}>
        {m[0]}
      </span>,
    );
    last = m.index + m[0].length;
  }
  if (last < line.length) out.push(line.slice(last));
  return out;
}
