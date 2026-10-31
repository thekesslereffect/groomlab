// SPDX-License-Identifier: Apache-2.0
import type { Metadata, Viewport } from 'next';
import localFont from 'next/font/local';
import { Provider } from '@/components/provider';
import { siteConfig } from '@/site.config';
import './global.css';

// Google Sans Flex (SIL Open Font License; app/fonts/README.md), its latin subset in the repo so the
// build never fetches it (next/font/google broke the Vercel build on 2026-10-31). The variable
// font: weight 1-1000 and the optical-size axis (big headlines the tighter display cut, body text
// the text cut).
const sans = localFont({
  src: './fonts/GoogleSansFlex-latin.woff2',
  weight: '1 1000',
  style: 'normal',
  display: 'swap',
  variable: '--font-site',
  // No fallback metrics for this family; the system stack in global.css covers the swap.
  adjustFontFallback: false,
});

export const metadata: Metadata = {
  metadataBase: new URL(siteConfig.url),
  title: {
    default: `${siteConfig.name}: ${siteConfig.tagline}`,
    template: `%s | ${siteConfig.name}`,
  },
  description: siteConfig.description,
  openGraph: {
    title: siteConfig.name,
    description: siteConfig.description,
    siteName: siteConfig.name,
    type: 'website',
  },
};

export const viewport: Viewport = {
  themeColor: '#0b0c0f',
  colorScheme: 'light dark',
};

export default function Layout({ children }: LayoutProps<'/'>) {
  return (
    <html lang="en" className={sans.variable} suppressHydrationWarning>
      <body className="flex min-h-screen flex-col font-sans antialiased">
        <Provider>{children}</Provider>
      </body>
    </html>
  );
}
