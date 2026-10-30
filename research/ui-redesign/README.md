# GroomLab UI lab

A clickable mock of the app's Groom workspace (React + Tailwind v4 + shadcn-style components on
Radix), built to try a cleaner, more modern UI and pick between options before touching the Qt
code. Everything here is meant to be buildable with Qt widgets + QSS: see [QT.md](QT.md).

## Run

```bash
npm install
npm run dev
```

Open http://localhost:5199 (or start `ui-lab` from `.claude/launch.json`). The app's icons and
fonts are read straight from `hosts/app/icons` and `hosts/app/fonts`, so the mock shows what the app ships.

## Use

The **Design lab** panel on the right flips every option live:

- **Directions**: curated combinations to compare (Chosen = the default, Today, Quiet docks, Rail, Studio, Graphite).
  *Today* is close to the current app, for comparison.
- **Look / Layout / Controls**: one row per design choice. Click a choice, or focus a row and use
  ← / →. A blue dot marks rows changed from the default. Hover a row for its Qt mapping.
- **A / B**: set A, change things, set B, then press **X** to flip between them.
- **Snapshots**: name and save a combination (kept in this browser).
- **Copy link**: the URL hash holds the options (`#theme=umber&layout=rail`), so a link reproduces
  a design. `#direction=rail` starts from a direction; `&graph=1` opens the Graph Editor (`&graph=2`: maximized);
  `&lab=0` hides the lab panel; `&page=components` / `&page=export`.
- **App / Parts / Qt** tabs: the workspace, every component in its states with its Qt widget,
  and the current design exported as a theme JSON for `hosts/maya/themes/` plus a reference QSS.
- Press **`** to hide the lab panel.

`npm run shots` (with the dev server running) writes a screenshot of every direction, with and
without the Graph Editor, to `shots/` for side-by-side review. `npm run shots -- rail` for just some.

## Add an option

1. `src/lab/options.ts`: add a choice to a dimension, or a new dimension (id, label, group,
   choices, and a `qt` note saying how Qt would do it).
2. Branch on it where it matters: `useOpt("tools") === "list"`. Colors, sizes and corners come
   from tokens (`src/theme/tokens.ts`), so a new theme or accent is one entry there.
3. Optionally add it to a direction at the bottom of `options.ts`.

## Rules that keep it translatable

- Colors only through the token classes (`bg-panel`, `bg-raised`, `text-dim`, `border-line`,
  `bg-accent`…), never raw Tailwind palette colors. Each token is a key in the theme JSON.
- No shadows, blur, gradients on widgets, or transitions (`.gl-ui` turns shadows and transitions off).
- Sizes come from the density tokens (`h-row`, `p-pad`, `gap-gap`, `text-ui`, `text-sm-ui`).
- Anything that isn't a stock Qt widget must be a small custom-painted widget (the fill field,
  the switch). QT.md lists the ones the mock uses.

## Layout

```
src/lab/          options registry, store (hash + localStorage), the lab panel
src/theme/        tokens (themes, accents, density) and the Qt export (theme JSON + QSS)
src/components/ui shadcn-style parts: button, tabs, select, toggle, field (sliders/rows), section, tooltip
src/workspace/    the mock app: shell layouts, menu bar, tools, properties, graph, viewport
src/data/         mock content from engine/ui and brush_catalog.cpp
src/gallery/      Parts and Qt pages
```

`public/viewport.png` is the app's own render of `web/public/samples/manny_hair.groom`
(`GroomLab.exe … --screenshot`).
