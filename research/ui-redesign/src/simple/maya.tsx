// SPDX-License-Identifier: Apache-2.0
// Maya's own chrome around the GroomLab parts: main menu bar (with the GroomLab menu), status
// line, shelf, tool box, the viewport's panel menus, the Attribute Editor, the time slider, the
// command and help lines. Maya's grays here are fixed colors, not GroomLab tokens: this is Maya,
// not us, and GroomLab's panels sit inside it.
import { ChevronRight, Circle, Move, MousePointer2, RotateCw, Scale3d, Lasso, Brush, Play, SkipBack, SkipForward, Search } from "lucide-react";
import { DropdownMenu } from "radix-ui";
import { useState, type ReactNode } from "react";
import { cn } from "@/lib/utils";
import { icon } from "@/data/icons";
import { groomLabMenu, type MenuItem } from "./data";
import { useS } from "./state";

const M = {
  bg: "bg-[#444444]", dark: "bg-[#373737]", darker: "bg-[#2b2b2b]", line: "border-[#2b2b2b]", text: "text-[#cccccc]", dim: "text-[#9a9a9a]",
  hi: "bg-[#5285a6]", field: "bg-[#2b2b2b]",
};
const mayaFont = "font-[Segoe_UI,Tahoma,sans-serif] text-[12px]";

// ---- main menu bar -------------------------------------------------------------------------------

function MenuEntries({ items, onPick }: { items: MenuItem[]; onPick: (i: MenuItem) => void }) {
  return (
    <>
      {items.map((it, i) =>
        it.sep ? (
          <DropdownMenu.Separator key={i} className="my-1 h-px bg-[#5a5a5a]" />
        ) : it.items ? (
          <DropdownMenu.Sub key={i}>
            <DropdownMenu.SubTrigger className="flex h-[22px] cursor-default items-center pl-6 pr-2 outline-none data-[highlighted]:bg-[#5285a6] data-[state=open]:bg-[#5285a6]">
              {it.label}<ChevronRight className="ml-auto size-3" />
            </DropdownMenu.SubTrigger>
            <DropdownMenu.Portal>
              <DropdownMenu.SubContent className={cn("z-50 min-w-52 border border-[#2b2b2b] py-1 text-[#dddddd]", M.dark, mayaFont)}>
                <MenuEntries items={it.items} onPick={onPick} />
              </DropdownMenu.SubContent>
            </DropdownMenu.Portal>
          </DropdownMenu.Sub>
        ) : (
          <DropdownMenu.Item key={i} onSelect={() => onPick(it)} className="flex h-[22px] cursor-default items-center gap-6 pl-6 pr-3 outline-none data-[highlighted]:bg-[#5285a6]">
            {it.label}{it.kbd && <span className="ml-auto text-[#9a9a9a]">{it.kbd}</span>}
          </DropdownMenu.Item>
        ),
      )}
    </>
  );
}

function MainMenu() {
  const s = useS();
  const maya = ["File", "Edit", "Create", "Select", "Modify", "Display", "Windows", "Mesh", "Edit Mesh", "Mesh Tools", "Deform", "UV", "Generate", "Cache", "Arnold"];
  const pick = (it: MenuItem) => {
    if (it.id === "search") s.setSearch(true);
    else if (it.id === "graph.open") s.setGraph(true);
    else if (it.id === "groom.create") { s.setHasGroom(true); s.say("Created groomLabShape1"); }
    else s.say(`GroomLab ▸ ${it.label.replace("…", "")}`);
  };
  return (
    <div className={cn("flex h-[22px] shrink-0 items-center gap-0 px-1", M.bg, M.text, mayaFont)}>
      <span className="mr-1 flex h-[18px] items-center gap-1 border border-[#2b2b2b] bg-[#3a3a3a] px-1.5 text-[11px]">Modeling <ChevronRight className="size-2.5 rotate-90" /></span>
      {maya.map((m) => <span key={m} className="px-[7px] hover:bg-[#5a5a5a]">{m}</span>)}
      <DropdownMenu.Root modal={false}>
        <DropdownMenu.Trigger asChild>
          <button className="px-[7px] font-semibold text-white outline-none hover:bg-[#5a5a5a] data-[state=open]:bg-[#5285a6]">GroomLab</button>
        </DropdownMenu.Trigger>
        <DropdownMenu.Portal>
          <DropdownMenu.Content align="start" className={cn("z-50 min-w-60 border border-[#2b2b2b] py-1 text-[#dddddd]", M.dark, mayaFont)}>
            <div className="flex h-3 items-center justify-center"><span className="h-px w-[90%] border-t border-dashed border-[#888]" /></div>
            <MenuEntries items={groomLabMenu} onPick={pick} />
          </DropdownMenu.Content>
        </DropdownMenu.Portal>
      </DropdownMenu.Root>
      <span className="px-[7px]">Help</span>
    </div>
  );
}

// ---- status line + shelf -------------------------------------------------------------------------

function StatusLine() {
  return (
    <div className={cn("flex h-[26px] shrink-0 items-center gap-1 border-t px-1", M.bg, M.line)}>
      {Array.from({ length: 22 }).map((_, i) => (
        <span key={i} className={cn("size-[18px] rounded-[2px]", i % 6 === 5 ? "mx-1 w-px bg-[#2b2b2b]" : "bg-[#555]")} />
      ))}
      <span className={cn("ml-auto flex h-[18px] w-48 items-center gap-1 px-1.5", M.field, M.dim)}><Search className="size-3" />Search Maya</span>
    </div>
  );
}

/** A GroomLab shelf (new: today there is none). Maya users look for a tool on a shelf first. */
function Shelf() {
  const s = useS();
  const tabs = ["Curves / Surfaces", "Poly Modeling", "Sculpting", "Rigging", "Animation", "Rendering", "FX", "XGen", "GroomLab"];
  const buttons: [string, string, () => void][] = [
    ["Groom Tools", "ui_tools", () => s.say("Groom Tools")],
    ["Graph Editor", "ui_graph", () => s.setGraph(!s.graph)],
    ["Create Groom from Selection", "ui_plus", () => { s.setHasGroom(true); s.say("Created groomLabShape1"); }],
    ["Save Groom File", "ui_save", () => s.say("Saved manny_hair.groom")],
    ["Simulate Range", "ui_play", () => s.say("Simulating 1-120…")],
    ["Export Alembic", "ui_export", () => s.say("Exported manny_hair.abc")],
    ["Find Command", "ui_help", () => s.setSearch(true)],
  ];
  return (
    <div className={cn("shrink-0 border-t", M.line, M.bg)}>
      <div className={cn("flex h-[20px] items-end gap-px px-1", mayaFont)}>
        {tabs.map((t) => (
          <span key={t} className={cn("px-2 text-[11px]", t === "GroomLab" ? "bg-[#5d5d5d] text-white" : "bg-[#3d3d3d] text-[#a8a8a8]")}>{t}</span>
        ))}
      </div>
      <div className="flex h-[38px] items-center gap-1 bg-[#5d5d5d] px-1.5">
        {buttons.map(([tip, glyph, fn]) => (
          <button key={tip} title={tip} onClick={fn} className="flex size-8 items-center justify-center rounded-[3px] bg-[#2f2f35] text-[#b9a4ff] outline-none hover:bg-[#3e3e46]">
            <span className="icon-mono size-[18px]" style={{ ["--icon" as string]: `url("${icon(glyph)}")` }} />
          </button>
        ))}
      </div>
    </div>
  );
}

function ToolBox() {
  const icons = [MousePointer2, Lasso, Brush, Move, RotateCw, Scale3d];
  return (
    <div className={cn("flex w-[34px] shrink-0 flex-col items-center gap-1 border-r py-1.5", M.bg, M.line)}>
      {icons.map((I, i) => <span key={i} className={cn("flex size-[26px] items-center justify-center text-[#cfcfcf]", i === 0 && "bg-[#5285a6]")}><I className="size-4" /></span>)}
      <span className="my-1 h-px w-5 bg-[#2b2b2b]" />
      {[0, 1, 2, 3].map((i) => <span key={i} className="size-[22px] bg-[#555]" />)}
    </div>
  );
}

// ---- viewport panel ------------------------------------------------------------------------------

export function PanelMenus({ children }: { children?: ReactNode }) {
  return (
    <div className={cn("flex h-[22px] shrink-0 items-center gap-0 px-1", M.bg, M.text, mayaFont)}>
      {["View", "Shading", "Lighting", "Show", "Renderer", "Panels"].map((m) => <span key={m} className="px-[6px]">{m}</span>)}
      <div className="ml-3 flex items-center gap-1">{Array.from({ length: 14 }).map((_, i) => <span key={i} className="size-[16px] rounded-[2px] bg-[#555]" />)}</div>
      {children}
    </div>
  );
}

// ---- Attribute Editor ----------------------------------------------------------------------------

function Frame({ title, open: initial = true, children, note }: { title: string; open?: boolean; children?: ReactNode; note?: string }) {
  const [open, setOpen] = useState(initial);
  return (
    <div className="mb-px">
      <button onClick={() => setOpen(!open)} className="flex h-[22px] w-full items-center gap-1 bg-[#5d5d5d] px-1 text-left font-semibold text-[#e0e0e0] outline-none">
        <ChevronRight className={cn("size-3", open && "rotate-90")} />{title}
        {note && <span className="ml-auto mr-1 font-normal text-[#c9b8ff]">{note}</span>}
      </button>
      {open && <div className="flex flex-col gap-[3px] py-1.5 pl-2 pr-1.5">{children}</div>}
    </div>
  );
}

function Attr({ label, value, slider, check }: { label: string; value?: string; slider?: number; check?: boolean }) {
  return (
    <div className="grid grid-cols-[130px_1fr] items-center gap-2">
      <span className="truncate text-right text-[#c8c8c8]">{label}</span>
      {check !== undefined ? (
        <span className="flex items-center gap-1.5"><span className={cn("size-3 border border-[#222]", check ? "bg-[#cfcfcf]" : "bg-[#2b2b2b]")} /></span>
      ) : (
        <span className="flex items-center gap-1.5">
          <span className="h-[18px] w-16 shrink-0 bg-[#2b2b2b] px-1 text-right leading-[18px] text-[#e0e0e0]">{value}</span>
          {slider !== undefined && (
            <span className="relative h-[6px] flex-1 bg-[#2b2b2b]"><span className="absolute -top-[3px] size-3 rounded-full bg-[#cfcfcf]" style={{ left: `calc(${slider * 100}% - 6px)` }} /></span>
          )}
        </span>
      )}
    </div>
  );
}

export function AttributeEditor() {
  const s = useS();
  return (
    <div className={cn("flex h-full w-[360px] shrink-0 flex-col border-l", M.bg, M.line, M.text, mayaFont)}>
      <div className="flex h-[22px] items-end gap-px border-b border-[#2b2b2b] px-1">
        {["groomLabShape1", "manny_hair", "head_geo"].map((t, i) => (
          <span key={t} className={cn("px-2 text-[11px]", i === 0 ? "bg-[#5d5d5d] text-white" : "bg-[#3d3d3d] text-[#a8a8a8]")}>{t}</span>
        ))}
      </div>
      <div className="flex items-center gap-2 px-2 py-1.5"><span className="text-[#9a9a9a]">groomLabShape:</span><span className="flex-1 bg-[#2b2b2b] px-1">groomLabShape1</span></div>
      <div className="gl-scroll flex-1">
        {s.hasGroom ? (
          <>
            <Frame title="Groom">
              <div className="grid grid-cols-2 gap-1 pl-[138px]">
                <button onClick={() => s.say("Groom Tools")} className="h-[22px] bg-[#5d5d5d] hover:bg-[#6a6a6a]">Groom Tools</button>
                <button onClick={() => s.setGraph(true)} className="h-[22px] bg-[#5d5d5d] hover:bg-[#6a6a6a]">Graph Editor</button>
              </div>
              <Attr label="Preview Density" value="0.500" slider={0.5} />
              <Attr label="Point Budget (M)" value="0.000" />
              <Attr label="Out Strand Count" value="25843" />
              <Attr label="Locked" check={false} />
            </Frame>
            <Frame title="Inputs">
              <Attr label="Scalp" value="head_geo" />
              <Attr label="Guides" value="104 · 3 groups" />
              <Attr label="Groom File" value="manny_hair.groom" />
              <Attr label="Follow Scalp" check={false} />
              <Attr label="Scale Source" value="Surface Area" />
              <Attr label="Groom Scale" value="1.000" />
            </Frame>
            <Frame title="Controls" note="from the graph">
              <Attr label="Clump Strength" value="0.800" slider={0.8} />
              <Attr label="Curl Radius" value="0.450" slider={0.45} />
              <span className="pl-[138px] text-[11px] text-[#9a9a9a]">Params promoted in the Graph Editor (right-click › Promote).</span>
            </Frame>
            <Frame title="Display" open={false}>
              <Attr label="Strand Colors" value="Solid" />
              <Frame title="Viewport Quality" open={false}>
                <Attr label="Hair Shading" check />
                <Attr label="Self Shadowing" check />
                <Attr label="Shadows" check={false} />
                <Attr label="Width Coverage" check />
                <Attr label="Clump LOD" check />
              </Frame>
            </Frame>
            <Frame title="Rendering" open={false}>
              <Attr label="Render Mode" value="curves" />
              <Frame title="Level of Detail" open={false}>
                <Attr label="LOD Distance" check={false} />
                <Attr label="Camera Cull" check={false} />
              </Frame>
            </Frame>
            <div className="m-2 flex flex-col gap-1 border border-dashed border-[#6a6a6a] p-2 text-[11px] leading-[15px] text-[#a8a8a8]">
              <b className="text-[#cfcfcf]">Moved out of the AE (about 64 controls → 35; 3 sections open, was 5):</b>
              <span>· Hair Color, Use Material Color, Display Color → the <b className="text-[#cfcfcf]">HairColor</b> node</span>
              <span>· Game Cards (display, textures, density) → the <b className="text-[#cfcfcf]">Cards</b> node</span>
              <span>· Render Overrides (density, width, length) → Cull / Width / Transform nodes set to <b className="text-[#cfcfcf]">Evaluate ▸ Render Only</b></span>
              <span>· Display ▸ Guides, Display Hair/Guides/Flow/Tubes/Instances, Isolate Group → the Groom Tools eyes (guides ▾)</span>
              <span>· Sculpting → the Correctives section (display while sculpting) and the Select card (soft radius)</span>
              <span>· Rendering ▸ Groom File → the Inputs' Groom File row (it was there twice)</span>
            </div>
          </>
        ) : (
          <div className="p-3 text-[#9a9a9a]">Select an object.</div>
        )}
      </div>
      <div className="flex gap-1 border-t border-[#2b2b2b] p-1">
        {["Select", "Load Attributes", "Copy Tab"].map((b) => <span key={b} className="flex-1 bg-[#5d5d5d] py-0.5 text-center">{b}</span>)}
      </div>
    </div>
  );
}

// ---- time slider + command / help lines ----------------------------------------------------------

export function TimeAndHelp({ help }: { help: string }) {
  return (
    <div className={cn("shrink-0 border-t", M.line, M.bg, M.text, mayaFont)}>
      <div className="flex h-[30px] items-center gap-2 px-2">
        <div className="relative h-[22px] flex-1 bg-[#3a3a3a]">
          {Array.from({ length: 13 }).map((_, i) => (
            <span key={i} className="absolute bottom-0 h-2 w-px bg-[#7a7a7a]" style={{ left: `${(i / 12) * 100}%` }}><span className="absolute -top-3 left-0.5 text-[9px] text-[#9a9a9a]">{i * 10}</span></span>
          ))}
          <span className="absolute inset-y-0 w-[3px] bg-[#c8c8c8]" style={{ left: "8%" }} />
        </div>
        <span className="w-10 bg-[#2b2b2b] px-1 text-right">8</span>
        <div className="flex items-center gap-1 text-[#cfcfcf]"><SkipBack className="size-3.5" /><Play className="size-3.5" /><SkipForward className="size-3.5" /></div>
      </div>
      <div className="flex h-[20px] items-center gap-2 border-t border-[#2b2b2b] px-2">
        <span className="font-semibold">MEL</span><span className="h-[16px] flex-1 bg-[#2b2b2b]" /><Circle className="size-3 text-[#9a9a9a]" />
      </div>
      <div className="flex h-[20px] items-center border-t border-[#2b2b2b] px-2 text-[#cfcfcf]">{help}</div>
    </div>
  );
}

export { MainMenu, StatusLine, Shelf, ToolBox };
