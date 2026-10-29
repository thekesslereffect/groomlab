// SPDX-License-Identifier: Apache-2.0
/**
 * The free download: the latest Maya build on Windows (one zip for Maya 2022 to 2027). One constant,
 * so a new host or file name is one edit.
 */
export const DOWNLOAD_URL = 'https://downloads.getgroomlab.com/maya/GroomLab-Maya-Windows-latest.zip';

/**
 * Everything about GroomLab that the site repeats in several places.
 * Change the download and donation links, the support address and the version here only.
 */
export const siteConfig = {
  name: 'GroomLab',
  tagline: 'The universal groom system.',
  description:
    'A free, universal groom system for hair, fur, feathers and scales: groom guides by hand, style with a node graph, render in Arnold and RenderMan and export the same groom to Unreal, Unity, USD and glTF. Runs in Maya 2022 to 2027 (2023 to 2027 in beta), with a standalone app and Blender in beta.',
  version: '0.35.0',

  // The deployed site's address (for social previews). NEXT_PUBLIC_SITE_URL overrides it.
  url: process.env.NEXT_PUBLIC_SITE_URL ?? 'https://getgroomlab.com',

  // One free download for everyone: GroomLab is free for any use, every feature and export included.
  downloadUrl: DOWNLOAD_URL,
  // Donations keep it going (the Support buttons, the footer, the apps' Help menus).
  donateUrl: 'https://ko-fi.com/getgroomlab',
  // The source: GroomLab is open source (Apache-2.0).
  repoUrl: 'https://github.com/thekesslereffect/groomlab',
  // The community: questions, work in progress, feedback.
  discordUrl: 'https://discord.gg/sr2rxjRVkm',
  supportEmail: 'support@getgroomlab.com',

  requirements: {
    maya: 'Maya 2022 (2023 to 2027 in beta)',
    os: 'Windows (64-bit)',
    arnold: 'MtoA 5.x',
    redist: 'Microsoft Visual C++ Redistributable 2015-2022 (x64)',
  },
} as const;

export type SiteConfig = typeof siteConfig;
