// SPDX-License-Identifier: Apache-2.0
// The app's own icons (hosts/app/icons), so the mock always shows what the Qt app ships.
const files = import.meta.glob("../../../../hosts/app/icons/*.svg", { eager: true, query: "?url", import: "default" }) as Record<string, string>;

export function icon(name: string): string {
  const key = Object.keys(files).find((k) => k.endsWith(`/${name}.svg`));
  return key ? files[key] : "";
}
