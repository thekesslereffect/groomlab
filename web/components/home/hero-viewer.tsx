// SPDX-License-Identifier: Apache-2.0
'use client';
import { Move } from 'lucide-react';
import { HERO_GROOM } from './engine-grooms';
import { StrandViewport } from './strand-viewport';

/*
 * The hero's ball of curls: big, in 3D, standing on the page with no frame, grown once by the
 * GroomLab engine in the browser (webhero.groom, as groomed in Maya). Under it on the right, a hint
 * that a drag turns it.
 */

const engine = { source: HERO_GROOM };

export function HeroViewer() {
  return (
    <div className="relative h-[min(104vw,460px)] sm:h-[clamp(460px,52vw,560px)] lg:h-[clamp(520px,44vw,640px)]">
      <StrandViewport
        pitch={0.14}
        fill={1}
        yaw={2.45}
        skin={[0.17, 0.16, 0.16]}
        framing={[[0, 0, 0], 1.75]}
        engine={engine}
        label="A ball of brown curls in 3D; drag to turn it"
      />
      {/* A drag turns it. */}
      <div
        aria-hidden="true"
        className="pointer-events-none absolute right-[var(--gutter)] bottom-2 flex flex-col items-center gap-3 sm:right-0 sm:bottom-6"
      >
        <span className="grid size-11 place-items-center rounded-full border border-white/25 text-gl-ink">
          <Move className="size-[18px]" strokeWidth={1.6} />
        </span>
        <span className="t-caps text-[10px] text-gl-ink-3">Drag to rotate</span>
      </div>
    </div>
  );
}
