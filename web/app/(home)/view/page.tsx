// SPDX-License-Identifier: Apache-2.0
import type { Metadata } from 'next';
import { GroomView } from '@/components/view/groom-view';

export const metadata: Metadata = {
  title: 'Groom viewer',
  description:
    'The GroomLab engine running in your browser (WebAssembly): open a .groom, move its sliders and download the edited file.',
};

/** A proof of concept: the real engine, built to WebAssembly, evaluating a groom in the page. */
export default function ViewPage() {
  return <GroomView sample="/samples/webhero.groom" />;
}
