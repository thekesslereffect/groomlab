// SPDX-License-Identifier: Apache-2.0
// Every building block in its states, with the Qt widget it becomes.
import { ChevronDown, Eye, Plus } from "lucide-react";
import { useState, type ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { FillField, Kbd, Param, Row, Swatch, TrackSlider } from "@/components/ui/field";
import { Section } from "@/components/ui/section";
import { Select } from "@/components/ui/select";
import { TabBar } from "@/components/ui/tabs";
import { Toggle } from "@/components/ui/toggle";
import { toolGroups } from "@/data/groom";
import { GuideGroups, ToolPalette } from "@/workspace/tools";

function Card({ title, qt, children, wide }: { title: string; qt: string; children: ReactNode; wide?: boolean }) {
  return (
    <div className={`flex flex-col rounded-pnl border border-line bg-panel ${wide ? "col-span-2" : ""}`}>
      <div className="border-b border-line px-pad py-1.5">
        <div className="font-semibold text-fg">{title}</div>
        <div className="text-sm-ui text-muted">Qt: {qt}</div>
      </div>
      <div className="flex flex-1 flex-col gap-gap p-pad">{children}</div>
    </div>
  );
}

export function Gallery() {
  const [tab, setTab] = useState("a");
  const [on, setOn] = useState(true);
  const [v, setV] = useState(0.4);
  return (
    <div className="gl-ui gl-scroll h-full bg-background p-4">
      <div className="mx-auto grid max-w-[1100px] grid-cols-3 gap-3">
        <Card title="Buttons" qt="QPushButton / QToolButton, [role] property per variant">
          <div className="flex flex-wrap gap-1.5">
            <Button>Default</Button>
            <Button variant="primary">Primary</Button>
            <Button variant="outline">Outline</Button>
            <Button variant="ghost">Ghost</Button>
            <Button variant="checked">Checked</Button>
            <Button disabled>Disabled</Button>
          </div>
          <div className="flex flex-wrap items-center gap-1.5">
            <Button size="sm">Small</Button>
            <Button size="sm">Menu<ChevronDown className="size-3" /></Button>
            <Button size="icon"><Plus className="size-4" /></Button>
            <Button size="iconSm" variant="ghost"><Eye className="size-4" /></Button>
          </div>
        </Card>

        <Card title="Tabs" qt="QTabBar, one QSS block per style">
          <TabBar value={tab} onChange={setTab} items={[{ id: "a", label: "Tools" }, { id: "b", label: "Paint" }, { id: "c", label: "Correctives" }]} />
          <span className="text-sm-ui text-muted">Switch the style under Controls › Tabs.</span>
        </Card>

        <Card title="On / off" qt="QCheckBox::indicator, or a painted switch">
          <Toggle checked={on} onChange={setOn} label="Follow Scalp" />
          <Toggle checked={!on} onChange={(x) => setOn(!x)} label="Locked" />
        </Card>

        <Card title="Value fields" qt="Painted drag field (fill) or QSlider + QLineEdit (track)">
          <FillField label="Strength" value={v} onChange={setV} />
          <FillField value={v} onChange={setV} />
          <TrackSlider value={v} onChange={setV} />
        </Card>

        <Card title="Property rows" qt="QFormLayout (split, stacked) — rows option">
          <Param label="Melanin" init={0.7} />
          <Row label="Mode"><Select value="Natural" options={["Natural", "Dyed", "Map"]} className="w-full" /></Row>
          <Row label="Dye"><Swatch color="#C8A27A" value="#C8A27A" /></Row>
        </Card>

        <Card title="Menu, select, keys" qt="QComboBox, QMenu, rich-text shortcut">
          <Select value="World X" options={["World X", "Topology", "Object X"]} />
          <div className="flex items-center gap-2 text-dim">Frame all <Kbd>F</Kbd> Undo <Kbd>Ctrl+Z</Kbd></div>
        </Card>

        <Card title="Sections" qt="Header QToolButton + body widget; cards = QFrame[role=card]" wide>
          <Section title="Hair Color">
            <Param label="Melanin" init={0.7} />
            <Param label="Redness" init={0.15} />
          </Section>
          <Section title="Display" open={false}><Param label="Width" init={1} /></Section>
        </Card>

        <Card title="List rows" qt="QListView + delegate (or QTreeWidget)">
          <GuideGroups />
        </Card>

        <Card title="Tool palette" qt="QToolButton grid in a QButtonGroup" wide>
          <div className="max-w-[300px]"><ToolPalette /></div>
        </Card>

        <Card title="Tool icons" qt="The app's SVGs (hosts/app/icons), recolored for Mono">
          <div className="grid grid-cols-6 gap-2 text-dim">
            {toolGroups.flatMap((g) => g.tools).slice(0, 12).map((t) => (
              <span key={t.id} className="icon-mono inline-block size-5" style={{ ["--icon" as string]: `url("${t.icon}")` }} />
            ))}
          </div>
        </Card>
      </div>
    </div>
  );
}
