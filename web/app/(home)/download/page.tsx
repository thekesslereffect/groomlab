// SPDX-License-Identifier: Apache-2.0
import type { Metadata } from 'next';
import { ArrowDownToLine, Heart } from 'lucide-react';
import { button } from '@/components/store-buttons';
import { TextLink } from '@/components/text-link';
import { cn } from '@/lib/cn';
import { siteConfig } from '@/site.config';

export const metadata: Metadata = {
  title: 'Download',
  description: 'Download GroomLab: free for everyone, for any use, with every feature and export.',
};

/**
 * What the publish scripts write next to each release: <host>/latest.json and, newest first,
 * <host>/releases.json (tools/publish_release.py for maya/, publish_blender.py for blender/,
 * publish_app.py for app/). macOS has its own beside them (latest-macos.json, releases-macos.json):
 * the platforms are released on their own, each at its own version.
 */
interface Release {
  version: string;
  url: string;
  sha256: string;
  size: number;
  date: string;
  notes?: string;
  channel?: string;
}

const DOWNLOADS = 'https://downloads.getgroomlab.com';

/**
 * A JSON file from the downloads bucket, read when the site is built (a static export); null when
 * it can't be reached. So a release is published to the bucket BEFORE main is pushed: the push's
 * build is what lists it.
 */
async function published<T>(path: string): Promise<T | null> {
  try {
    // A named client: Cloudflare turns away some default User-Agents.
    const response = await fetch(`${DOWNLOADS}/${path}`, {
      cache: 'force-cache',
      headers: { 'User-Agent': 'GroomLab-site/1' },
    });
    if (!response.ok) return null;
    return (await response.json()) as T;
  } catch {
    return null;
  }
}

/**
 * A platform's latest release and the ones before it: <host>/latest<suffix>.json and, newest first,
 * <host>/releases<suffix>.json (suffix "" for Windows, "-macos" for macOS).
 */
async function releases(host: string, suffix = ''): Promise<{ latest: Release | null; older: Release[] }> {
  const [latest, index] = await Promise.all([
    published<Release>(`${host}/latest${suffix}.json`),
    published<{ releases: Release[] }>(`${host}/releases${suffix}.json`),
  ]);
  const older = (index?.releases ?? []).filter((r) => r.version !== latest?.version);
  return { latest, older };
}

const noteLines = (notes?: string) =>
  (notes ?? '')
    .split('\n')
    .map((line) => line.replace(/^\s*-\s*/, '').trim())
    .filter(Boolean);

const megabytes = (bytes: number) => `${(bytes / 1e6).toFixed(1)} MB`;
const day = (iso: string) =>
  new Date(iso).toLocaleDateString('en-US', { year: 'numeric', month: 'long', day: 'numeric', timeZone: 'UTC' });

/** Newest first: by date, then by version number. */
const newestFirst = (a: Release, b: Release) =>
  (b.date ?? '').localeCompare(a.date ?? '') || b.version.localeCompare(a.version, undefined, { numeric: true });

/** One platform's build of a package: Windows and macOS are released on their own, at their own versions. */
interface Build {
  platform: string;
  release: Release | null;
  older: Release[];
  /** What the button downloads (Maya's Windows build: the latest zip's stable address). */
  download?: string;
  requires: string[];
}

interface Package {
  name: string;
  /** A beta is listed with a Beta badge; with no build published it shows as coming soon. */
  beta: boolean;
  what: string;
  includes?: string[];
  /** What every platform needs (the platform's own needs are its build's). */
  requires?: string[];
  docs?: { href: string; label: string };
  builds: Build[];
  /** Where a user of this package gets the next version (under Previous versions). */
  updates?: string;
}

/** A platform's build from its published releases. */
const build = (platform: string, r: { latest: Release | null; older: Release[] }, requires: string[], download?: string): Build => ({
  platform,
  release: r.latest,
  older: r.older,
  download: r.latest ? (download ?? r.latest.url) : undefined,
  requires,
});

export default async function DownloadPage() {
  const [mayaWin, mayaMac, blenderWin, blenderMac, appWin, appMac] = await Promise.all([
    releases('maya'),
    releases('maya', '-macos'),
    releases('blender'),
    releases('blender', '-macos'),
    releases('app'),
    releases('app', '-macos'),
  ]);
  const req = siteConfig.requirements;
  const mac = 'macOS 13.3 or later on Apple silicon';
  const packages: Package[] = [
    {
      name: 'GroomLab for Maya',
      beta: false,
      what: 'The plugin: Groom Tools, the Graph Editor, painting, cards and exports, rendered with Arnold or RenderMan.',
      includes: ['Arnold and RenderMan procedurals for render farms', 'groomlab-cli for caches and farms', 'Check for Updates in the GroomLab menu'],
      requires: [`Arnold renders: ${req.arnold}`],
      docs: { href: '/docs/installation', label: 'Install guide' },
      builds: [
        build('Windows', mayaWin, [req.maya, 'Windows 10 or 11 (64-bit)', req.redist], siteConfig.downloadUrl),
        build('macOS', mayaMac, ['Maya 2024 to 2027', mac, 'RenderMan: with RenderMan 28 for Apple silicon']),
      ],
      updates: 'Check for Updates in the GroomLab menu offers the latest for your platform.',
    },
    {
      name: 'GroomLab for Blender',
      beta: true,
      what: 'GroomLab’s tools and Graph Editor inside Blender, rendered in Cycles and EEVEE. Opens the same .groom files as Maya.',
      includes: ['A Blender extension: install the zip from disk', 'Updates through Blender’s extension repositories'],
      requires: ['Blender 5.2 LTS'],
      docs: { href: '/docs/blender', label: 'Install guide' },
      builds: [build('Windows', blenderWin, ['Windows 10 or 11 (64-bit)']), build('macOS', blenderMac, [mac])],
      updates: 'Blender offers the latest once the GroomLab repository is added (see the install guide).',
    },
    {
      name: 'GroomLab app',
      beta: true,
      what: 'GroomLab on its own: open a scalp, groom, look-dev and export without a 3D package. Opens the same .groom files as Maya and Blender.',
      includes: ['No installer: unzip and run it', 'Check for Updates in the Help menu'],
      docs: { href: '/docs/app', label: 'App guide' },
      builds: [
        build('Windows', appWin, ['Windows 10 or 11 (64-bit)', 'A graphics card with Direct3D 11']),
        build('macOS', appMac, [mac]),
      ],
      updates: 'Help > Check for Updates in the app says when a newer version is out.',
    },
  ];

  return (
    <section aria-labelledby="download-title" className="wrap py-20 sm:py-28">
      <div className="mx-auto max-w-2xl text-center">
        <h1 id="download-title" className="display t-section">
          Download GroomLab
        </h1>
        <p className="mt-5 text-[17px] leading-snug text-gl-ink-2 text-pretty">
          Free and open source, for any use: every feature and every export.{' '}
          <a className="text-gl-blue-soft hover:underline" href={siteConfig.repoUrl}>
            Source on GitHub
          </a>
        </p>
        <div className="mt-8 flex justify-center">
          <a
            href={siteConfig.donateUrl}
            target="_blank"
            rel="noopener noreferrer"
            className={cn(button.base, button.md, button.quiet)}
          >
            Support GroomLab on Ko-fi
            <Heart aria-hidden="true" className="size-4 opacity-60" strokeWidth={2.4} />
          </a>
        </div>
      </div>

      <ul className="mx-auto mt-14 grid max-w-4xl gap-6 sm:mt-16">
        {packages.map((p) => {
          const shipped = p.builds.filter((b) => b.release);
          // What's new, once a version: platforms on the same version share it.
          const versions = new Map<string, { release: Release; platforms: string[] }>();
          for (const b of shipped) {
            const r = b.release!;
            const seen = versions.get(r.version);
            if (seen) seen.platforms.push(b.platform);
            else versions.set(r.version, { release: r, platforms: [b.platform] });
          }
          const notes = [...versions.values()].filter((v) => noteLines(v.release.notes).length > 0);
          const older = p.builds
            .flatMap((b) => b.older.map((release) => ({ platform: b.platform, release })))
            .sort((a, b) => newestFirst(a.release, b.release));
          return (
            <li
              key={p.name}
              className={cn(
                'rounded-[28px] p-6 sm:p-8',
                shipped.length ? 'border border-white/[0.08] bg-gl-mist' : 'border border-dashed border-white/[0.14]',
              )}
            >
              <div className="max-w-2xl">
                <div className="flex flex-wrap items-center gap-3">
                  <h2 className="text-[1.5rem] font-semibold tracking-tight">{p.name}</h2>
                  {p.beta && shipped.length > 0 && (
                    <span className="rounded-full bg-gl-yellow/15 px-3 py-1 text-[13px] font-semibold text-gl-yellow ring-1 ring-gl-yellow/40">
                      Beta
                    </span>
                  )}
                  {!shipped.length && (
                    <span className="rounded-full bg-white/[0.06] px-3 py-1 text-[13px] font-medium text-gl-ink-2">
                      Coming soon
                    </span>
                  )}
                </div>
                <p className="mt-3 text-[15px] leading-relaxed text-gl-ink-2 text-pretty">{p.what}</p>
              </div>

              {shipped.length > 0 && (
                <ul className="mt-6 grid gap-3 sm:grid-cols-2">
                  {p.builds.map((b) => (
                    <li
                      key={b.platform}
                      className={cn(
                        'flex flex-col gap-4 rounded-2xl p-5',
                        b.release ? 'bg-gl-paper ring-1 ring-white/[0.08]' : 'ring-1 ring-dashed ring-white/[0.12]',
                      )}
                    >
                      <div className="flex flex-wrap items-center gap-2">
                        <h3 className="text-[17px] font-semibold">{b.platform}</h3>
                        {b.release ? (
                          <span className="rounded-full bg-gl-blue/15 px-2.5 py-0.5 text-[12px] font-semibold text-gl-blue-soft ring-1 ring-gl-blue/30">
                            v{b.release.version}
                          </span>
                        ) : (
                          <span className="rounded-full bg-white/[0.06] px-2.5 py-0.5 text-[12px] font-medium text-gl-ink-2">
                            Coming soon
                          </span>
                        )}
                      </div>
                      <ul className="space-y-1 text-[14px] text-gl-ink-2">
                        {b.requires.map((r) => (
                          <li key={r}>{r}</li>
                        ))}
                      </ul>
                      {b.release && b.download && (
                        <div className="mt-auto">
                          <a href={b.download} className={cn(button.base, button.md, button.primary, 'w-full justify-center')}>
                            Download for {b.platform}
                            <ArrowDownToLine aria-hidden="true" className="size-4 opacity-80" strokeWidth={2.4} />
                          </a>
                          <p className="mt-2 text-center text-[13px] text-gl-ink-2">
                            {megabytes(b.release.size)} · {day(b.release.date)}
                          </p>
                          <p className="mt-1 text-center font-mono text-[11px] break-all text-gl-ink-3" title="SHA-256 of the zip">
                            SHA-256 {b.release.sha256}
                          </p>
                        </div>
                      )}
                    </li>
                  ))}
                </ul>
              )}

              {shipped.length > 0 && (
                <div className="mt-6 grid gap-6 border-t border-white/[0.07] pt-6 sm:grid-cols-2">
                  {p.includes && (
                    <div>
                      <h3 className="text-[13px] font-semibold tracking-wide text-gl-ink-2 uppercase">Includes</h3>
                      <ul className="mt-2 space-y-1 text-[15px] text-gl-ink">
                        {p.includes.map((i) => (
                          <li key={i}>{i}</li>
                        ))}
                      </ul>
                    </div>
                  )}
                  {p.requires && (
                    <div>
                      <h3 className="text-[13px] font-semibold tracking-wide text-gl-ink-2 uppercase">Requires</h3>
                      <ul className="mt-2 space-y-1 text-[15px] text-gl-ink">
                        {p.requires.map((r) => (
                          <li key={r}>{r}</li>
                        ))}
                      </ul>
                    </div>
                  )}
                  {notes.map((v) => (
                    <div key={v.release.version} className="sm:col-span-2">
                      <h3 className="text-[13px] font-semibold tracking-wide text-gl-ink-2 uppercase">
                        What&apos;s new in {v.release.version}
                        {versions.size > 1 && <span className="font-medium normal-case"> · {v.platforms.join(' and ')}</span>}
                      </h3>
                      <ul className="mt-2 space-y-1 text-[15px] leading-relaxed text-gl-ink">
                        {noteLines(v.release.notes).map((line) => (
                          <li key={line} className="flex gap-2">
                            <span aria-hidden="true" className="mt-[0.6em] size-1.5 shrink-0 rounded-full bg-gl-blue" />
                            <span>{line}</span>
                          </li>
                        ))}
                      </ul>
                    </div>
                  ))}
                </div>
              )}

              {older.length > 0 && (
                <details className="group mt-6 border-t border-white/[0.07] pt-5">
                  <summary className="cursor-pointer list-none text-[15px] font-semibold text-gl-blue-soft select-none hover:underline">
                    Previous versions ({older.length})
                    <span aria-hidden="true" className="ml-1 inline-block transition-transform group-open:rotate-90">
                      ›
                    </span>
                  </summary>
                  <ul className="mt-4 space-y-4">
                    {older.map(({ platform, release: r }) => (
                      <li key={`${platform}-${r.version}`} className="rounded-2xl bg-gl-paper p-4 ring-1 ring-white/[0.08]">
                        <div className="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                          <div>
                            <p className="font-semibold">
                              v{r.version} <span className="font-medium text-gl-ink-2">· {platform}</span>
                            </p>
                            <p className="text-[13px] text-gl-ink-2">
                              {megabytes(r.size)} · {day(r.date)}
                            </p>
                          </div>
                          <a
                            href={r.url}
                            className="inline-flex items-center gap-1.5 text-[15px] font-medium text-gl-blue-soft hover:underline"
                          >
                            Download v{r.version}
                            <ArrowDownToLine aria-hidden="true" className="size-4" strokeWidth={2.4} />
                          </a>
                        </div>
                        {noteLines(r.notes).length > 0 && (
                          <ul className="mt-3 space-y-1 text-[14px] leading-relaxed text-gl-ink-2">
                            {noteLines(r.notes).map((line) => (
                              <li key={line}>{line}</li>
                            ))}
                          </ul>
                        )}
                        <p className="mt-2 font-mono text-[11px] break-all text-gl-ink-3">SHA-256 {r.sha256}</p>
                      </li>
                    ))}
                  </ul>
                  <p className="mt-3 text-[13px] text-gl-ink-2">
                    Older versions are as they were released: they don&apos;t get fixes. {p.updates}
                  </p>
                </details>
              )}

              {p.docs && (
                <div className="mt-6">
                  <TextLink href={p.docs.href}>{p.docs.label}</TextLink>
                </div>
              )}
            </li>
          );
        })}
      </ul>

      <p className="mx-auto mt-10 max-w-2xl text-center text-sm text-gl-ink-2">
        The Arnold and RenderMan procedurals and groomlab-cli in the Maya download render and cache GroomLab grooms on
        any machine. Questions? Write to{' '}
        <a className="text-gl-blue-soft hover:underline" href={`mailto:${siteConfig.supportEmail}`}>
          {siteConfig.supportEmail}
        </a>
        .
      </p>
    </section>
  );
}
