// SPDX-License-Identifier: Apache-2.0
// Screenshots of every direction (and a few variants) with headless Edge/Chrome, for side-by-side
// review: `npm run dev` in one terminal, then `npm run shots` (or `npm run shots -- rail studio`).
// Writes shots/<name>.png (gitignored).
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

const root = path.dirname(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1")));
const out = path.join(root, "shots");
mkdirSync(out, { recursive: true });

const browsers = [
  process.env.BROWSER,
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/usr/bin/google-chrome",
].filter(Boolean);
const browser = browsers.find((b) => existsSync(b));
if (!browser) throw new Error("No Edge or Chrome found: set BROWSER=path/to/chrome");

// The directions' ids, read from options.ts so new ones show up without editing this file.
const src = readFileSync(path.join(root, "src/lab/options.ts"), "utf8");
const directions = [...src.slice(src.indexOf("export const directions")).matchAll(/\bid: "([\w-]+)", label:/g)].map((m) => m[1]);

const shots = {
  ...Object.fromEntries(directions.map((d) => [d, `direction=${d}`])),
  ...Object.fromEntries(directions.map((d) => [`${d}-graph`, `direction=${d}&graph=1`])),
  ...Object.fromEntries(directions.map((d) => [`${d}-nodes`, `direction=${d}&graph=2`])),
  "quiet-paper": "direction=quiet&theme=paper",
  parts: "direction=quiet&page=components",
};

const pick = process.argv.slice(2);
const url = process.env.LAB_URL ?? "http://localhost:5199/";
for (const [name, hash] of Object.entries(shots)) {
  if (pick.length && !pick.some((p) => name.startsWith(p))) continue;
  const file = path.join(out, `${name}.png`);
  execFileSync(browser, [
    "--headless=new", "--disable-gpu", "--hide-scrollbars", "--virtual-time-budget=4000",
    `--user-data-dir=${mkdtempSync(path.join(tmpdir(), "gl-shot-"))}`,
    "--window-size=1600,960", `--screenshot=${file}`, `${url}#${hash}&lab=0`,
  ], { stdio: "ignore" });
  console.log(`shots/${name}.png`);
}
