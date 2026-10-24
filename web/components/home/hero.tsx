// SPDX-License-Identifier: Apache-2.0
import { ArrowRight } from 'lucide-react';
import Link from 'next/link';
import type { CSSProperties } from 'react';
import { button } from '@/components/store-buttons';
import { cn } from '@/lib/cn';
import { siteConfig } from '@/site.config';
import { HeroViewer } from './hero-viewer';
import { TunePanel } from './tune-panel';

const delay = (s: number) => ({ '--d': `${s}s` }) as CSSProperties;

/** The opening, split in two: the promise on the left, a ball of curls in 3D on the right. */
export function Hero() {
  return (
    <section id="hero" aria-labelledby="hero-title" className="relative overflow-x-clip">
      <div className="wrap-wide grid items-center gap-2 pt-12 pb-10 sm:pt-16 lg:grid-cols-[minmax(0,1fr)_minmax(0,1.08fr)] lg:gap-6 lg:pt-6 lg:pb-8">
        <div className="relative z-10 lg:py-20">
          <p className="rise t-caps flex items-center gap-4 text-gl-ink-2" style={delay(0)}>
            <span aria-hidden="true" className="h-px w-8 bg-gl-blue" />
            The Universal Groom System
          </p>
          <h1 id="hero-title" className="rise display t-hero-split mt-6" style={delay(0.06)}>
            Groom once.
            <br />
            Use it everywhere.
          </h1>
          <p className="rise t-lede mt-6 max-w-[33rem] text-gl-ink-2" style={delay(0.12)}>
            Hair, fur, feathers and scales, groomed with brushes and a node graph in Maya, Blender or the GroomLab app,
            and the same in Arnold, V-Ray, RenderMan, Unreal and Unity.
          </p>
          <div className="rise mt-9 flex flex-col gap-3 sm:flex-row sm:gap-4" style={delay(0.18)}>
            <Link href="/download" className={cn(button.base, button.lg, button.primary)}>
              Get GroomLab
              <ArrowRight aria-hidden="true" className="size-[18px]" strokeWidth={2.2} />
            </Link>
            <a href="#try" className={cn(button.base, button.lg, button.outline)}>
              <svg aria-hidden="true" viewBox="0 0 20 20" className="size-5" fill="none">
                <circle cx="10" cy="10" r="8.6" stroke="currentColor" strokeWidth="1.5" />
                <path d="M8.3 6.9v6.2l5-3.1-5-3.1Z" fill="currentColor" />
              </svg>
              Try the node graph
            </a>
          </div>
          <p className="rise mt-6 text-[13px] text-gl-ink-3" style={delay(0.22)}>
            Free and open source · Apache-2.0 ·{' '}
            <Link href="/docs/whats-new" className="text-gl-ink-2 underline-offset-4 hover:text-gl-ink hover:underline">
              Early access {siteConfig.version}
            </Link>
          </p>
        </div>

        <div className="rise relative -mx-[var(--gutter)] sm:mx-0" style={delay(0.15)}>
          <HeroViewer />
          <TunePanel />
        </div>
      </div>
    </section>
  );
}
