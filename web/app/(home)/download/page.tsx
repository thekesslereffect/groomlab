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
 * publish_app.py for app/). A macOS release has its own <host>/latest-macos.json beside them.
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

/** The latest release and the ones before it (<host>/releases.json, newest first). */
async function releases(host: string): Promise<{ latest: Release | null; older: Release[] }> {
  const [latest, index] = await Promise.all([
    published<Release>(`${host}/latest.json`),
    published<{ releases: Release[] }>(`${host}/releases.json`),
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

interface Package {
  name: string;
  platform: string;
  /** A beta is listed like a release, with a Beta badge; it shows as coming soon until its latest.json is published. */
  status: 'available' | 'beta' | 'soon';
  what: string;
  includes?: string[];
  requires?: string[];
  docs?: { href: string; label: string };
  release?: Release | null;
  older?: Release[];
  download?: string;
  /** The macOS (Apple silicon) release, once its latest-macos.json is published: a second button. */
  mac?: Release | null;
  /** Where a user of this package gets the next version (under Previous versions). */
  updates?: string;
}

/** A beta's status: listed once its latest.json is published (the download is that release's file). */
const beta = (release: Release | null): Pick<Package, 'status' | 'release' | 'download'> =>
  release ? { status: 'beta', release, download: release.url } : { status: 'soon', release: null };

export default async function DownloadPage() {
  const [{ latest: maya, older: olderMaya }, { latest: blender, older: olderBlender }, { latest: app, older: olderApp }] =
    await Promise.all([releases('maya'), releases('blender'), releases('app')]);
  const [mayaMac, blenderMac, appMac] = await Promise.all(
    ['maya', 'blender', 'app'].map((host) => published<Release>(`${host}/latest-macos.json`)),
  );
  const req = siteConfig.requirements;
  const packages: Package[] = [
    {
      name: 'GroomLab for Maya',
      platform: `${req.maya} · ${req.os}`,
      status: 'available',
      what: 'The plugin: Groom Tools, the Graph Editor, painting, cards and exports, rendered with Arnold or RenderMan.',
      includes: ['Arnold and RenderMan procedurals for render farms', 'groomlab-cli for caches and farms', 'Check for Updates in the GroomLab menu'],
      requires: [req.maya, `Arnold renders: ${req.arnold}`, req.redist],
      docs: { href: '/docs/installation', label: 'Install guide' },
      release: maya,
      older: olderMaya,
      download: siteConfig.downloadUrl,
      mac: mayaMac,
      updates: 'Check for Updates in the GroomLab menu offers the latest.',
    },
    {
      name: 'GroomLab for Blender',
      platform: 'Blender 5.2 LTS · Windows (64-bit)',
      ...beta(blender),
      what: 'GroomLab’s tools and Graph Editor inside Blender, rendered in Cycles and EEVEE. Opens the same .groom files as Maya.',
      includes: ['A Blender extension: install the zip from disk', 'Updates through Blender’s extension repositories'],
      requires: ['Blender 5.2 LTS', req.os],
      docs: { href: '/docs/blender', label: 'Install guide' },
      older: olderBlender,
      mac: blenderMac,
      updates: 'Blender offers the latest once the GroomLab repository is added (see the install guide).',
    },
    {
      name: 'GroomLab app',
      platform: 'Windows 10 and 11 (64-bit)',
      ...beta(app),
      what: 'GroomLab on its own: open a scalp, groom, look-dev and export without a 3D package. Opens the same .groom files as Maya and Blender.',
      includes: ['No installer: unzip and run GroomLab.exe', 'Check for Updates in the Help menu'],
      requires: ['Windows 10 or 11 (64-bit)', 'A graphics card with Direct3D 11'],
      docs: { href: '/docs/app', label: 'App guide' },
      older: olderApp,
      mac: appMac,
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
        {packages.map((p) => (
          <li
            key={p.name}
            className={cn(
              'rounded-[28px] p-6 sm:p-8',
              p.status !== 'soon' ? 'border border-white/[0.08] bg-gl-mist' : 'border border-dashed border-white/[0.14]',
            )}
          >
            <div className="flex flex-col gap-6 sm:flex-row sm:items-start sm:justify-between">
              <div className="max-w-xl">
                <div className="flex flex-wrap items-center gap-3">
                  <h2 className="text-[1.5rem] font-semibold tracking-tight">{p.name}</h2>
                  {p.status !== 'soon' && p.release && (
                    <span className="rounded-full bg-gl-blue/15 px-3 py-1 text-[13px] font-semibold text-gl-blue-soft ring-1 ring-gl-blue/30">
                      v{p.release.version}
                    </span>
                  )}
                  {p.status === 'beta' && (
                    <span className="rounded-full bg-gl-yellow/15 px-3 py-1 text-[13px] font-semibold text-gl-yellow ring-1 ring-gl-yellow/40">
                      Beta
                    </span>
                  )}
                  {p.status === 'soon' && (
                    <span className="rounded-full bg-white/[0.06] px-3 py-1 text-[13px] font-medium text-gl-ink-2">
                      Coming soon
                    </span>
                  )}
                </div>
                <p className="mt-1 text-[15px] font-medium text-gl-ink-2">{p.platform}</p>
                <p className="mt-3 text-[15px] leading-relaxed text-gl-ink-2 text-pretty">{p.what}</p>
              </div>
              {((p.status !== 'soon' && p.download) || p.mac) && (
                <div className="flex shrink-0 flex-col items-stretch gap-2 sm:items-end">
                  {p.status !== 'soon' && p.download && (
                    <>
                      <a href={p.download} className={cn(button.base, button.md, button.primary)}>
                        {p.mac ? 'Windows' : 'Download'}
                        <ArrowDownToLine aria-hidden="true" className="size-4 opacity-80" strokeWidth={2.4} />
                      </a>
                      {p.release && (
                        <p className="text-center text-[13px] text-gl-ink-2 sm:text-right">
                          {megabytes(p.release.size)} · {day(p.release.date)}
                        </p>
                      )}
                    </>
                  )}
                  {p.mac && (
                    <>
                      <a href={p.mac.url} className={cn(button.base, button.md, button.primary)}>
                        macOS (Apple silicon)
                        <ArrowDownToLine aria-hidden="true" className="size-4 opacity-80" strokeWidth={2.4} />
                      </a>
                      <p className="text-center text-[13px] text-gl-ink-2 sm:text-right">
                        {megabytes(p.mac.size)} · {day(p.mac.date)} · macOS 13.3 or later
                      </p>
                    </>
                  )}
                </div>
              )}
            </div>

            {p.status !== 'soon' && (
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
                {p.release?.notes && (
                  <div className="sm:col-span-2">
                    <h3 className="text-[13px] font-semibold tracking-wide text-gl-ink-2 uppercase">
                      What&apos;s new in {p.release.version}
                    </h3>
                    <ul className="mt-2 space-y-1 text-[15px] leading-relaxed text-gl-ink">
                      {noteLines(p.release.notes).map((line) => (
                          <li key={line} className="flex gap-2">
                            <span aria-hidden="true" className="mt-[0.6em] size-1.5 shrink-0 rounded-full bg-gl-blue" />
                            <span>{line}</span>
                          </li>
                        ))}
                    </ul>
                  </div>
                )}
              </div>
            )}

            {p.older && p.older.length > 0 && (
              <details className="group mt-6 border-t border-white/[0.07] pt-5">
                <summary className="cursor-pointer list-none text-[15px] font-semibold text-gl-blue-soft select-none hover:underline">
                  Previous versions ({p.older.length})
                  <span aria-hidden="true" className="ml-1 inline-block transition-transform group-open:rotate-90">
                    ›
                  </span>
                </summary>
                <ul className="mt-4 space-y-4">
                  {p.older.map((r) => (
                    <li key={r.version} className="rounded-2xl bg-gl-paper p-4 ring-1 ring-white/[0.08]">
                      <div className="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                        <div>
                          <p className="font-semibold">v{r.version}</p>
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

            <div className="mt-6 flex flex-wrap items-center gap-x-6 gap-y-2">
              {p.docs && <TextLink href={p.docs.href}>{p.docs.label}</TextLink>}
              {p.release && (
                <p className="font-mono text-[12px] break-all text-gl-ink-3" title="SHA-256 of the zip">
                  SHA-256 {p.release.sha256}
                </p>
              )}
            </div>
          </li>
        ))}
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
