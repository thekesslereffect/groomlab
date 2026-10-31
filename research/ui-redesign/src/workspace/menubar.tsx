// SPDX-License-Identifier: Apache-2.0
// Qt: QMenuBar + QMenu (QSS QMenuBar::item:selected, QMenu::item:selected, QMenu::separator);
// the right side is QMenuBar::setCornerWidget(a QWidget with two QToolButtons).
import { Menubar as M } from "radix-ui";
import { ChevronDown } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Kbd } from "@/components/ui/field";
import { icon } from "@/data/icons";
import { useOpt } from "@/lab/store";

const menus: Record<string, (string | [string, string])[]> = {
  File: [["New Groom on Mesh…", "Ctrl+N"], ["Open Groom…", "Ctrl+O"], "Open Recent", "-", ["Save", "Ctrl+S"], "Save As…", "Collect Files…", "-", "Import", "Export", "-", ["Quit", "Ctrl+Q"]],
  Edit: [["Undo Comb", "Ctrl+Z"], ["Redo", "Ctrl+Y"], "-", ["Select Guides", "S"], ["Delete Selected Guides", "Del"], "-", "Preferences…"],
  View: ["Mode", "Theme", "-", ["Frame All", "F"], "Cameras", "-", "Graph Editor", "Groom Tools", "Properties", "Timeline"],
  GroomLab: ["Groom Tools", "Graph Editor", "-", "Simulate", "Transfer…", "Studio Lighting", "-", "Render", "Bake Cards…"],
  Help: ["Documentation", ["Shortcuts", "?"], "-", "Check for Updates", "About GroomLab"],
};

export function MenuBar() {
  const output = useOpt("output");
  return (
    <div className="flex h-[calc(var(--gl-row)+6px)] shrink-0 items-center border-b border-line bg-panel px-1.5">
      <img src={icon("ui_logo")} className="mx-1.5 size-4 opacity-90" alt="" />
      <M.Root className="flex">
        {Object.entries(menus).map(([name, items]) => (
          <M.Menu key={name}>
            <M.Trigger className="h-[calc(var(--gl-row)-2px)] rounded-ctl px-2.5 text-ui text-dim outline-none hover:bg-hover hover:text-fg data-[state=open]:bg-hover data-[state=open]:text-fg">
              {name}
            </M.Trigger>
            <M.Portal>
              <M.Content align="start" sideOffset={4} className="gl-ui z-50 min-w-52 rounded-ctl border border-line bg-raised p-1 text-ui text-fg">
                {items.map((it, i) =>
                  it === "-" ? (
                    <M.Separator key={i} className="mx-1 my-1 h-px bg-line" />
                  ) : (
                    <M.Item key={i} className="flex h-row cursor-default items-center rounded-[calc(var(--gl-radius)-2px)] px-2 outline-none data-[highlighted]:bg-selection">
                      <span className="flex-1">{typeof it === "string" ? it : it[0]}</span>
                      {typeof it !== "string" && <Kbd>{it[1]}</Kbd>}
                    </M.Item>
                  ),
                )}
              </M.Content>
            </M.Portal>
          </M.Menu>
        ))}
      </M.Root>
      <div className="flex-1" />
      <span className="mr-3 text-sm-ui text-muted">manny_hair.groom</span>
      {output === "corner" && (
        <div className="flex items-center gap-1">
          <Button size="sm" variant="ghost">Render<ChevronDown className="size-3" /></Button>
          <Button size="sm" variant="primary">Export<ChevronDown className="size-3" /></Button>
        </div>
      )}
    </div>
  );
}
