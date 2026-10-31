// SPDX-License-Identifier: Apache-2.0
// The Properties dock: the groom's settings in engine/ui/properties.json's section order.
import { Box, FileText, Link2, Pencil, Plus, RotateCw, Save, Shield, Spline } from "lucide-react";
import { useState, type ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { Param, Row, Swatch, TextValue } from "@/components/ui/field";
import { Section } from "@/components/ui/section";
import { Select } from "@/components/ui/select";
import { TabBar } from "@/components/ui/tabs";
import { Toggle } from "@/components/ui/toggle";
import { Tip } from "@/components/ui/tooltip";
import { useWS } from "./state";

function Bool({ label, init = false }: { label: string; init?: boolean }) {
  const [v, setV] = useState(init);
  return <Row label={label}><Toggle checked={v} onChange={setV} /></Row>;
}

function Choice({ label, value, options }: { label: string; value: string; options: string[] }) {
  const [v, setV] = useState(value);
  return <Row label={label}><Select value={v} options={options} onChange={setV} className="w-full" /></Row>;
}

/** An input the groom reads (scalp, guides, collider...): what it is, then one action. */
function Input({ icon, label, value, empty, action }: { icon: ReactNode; label: string; value?: string; empty?: string; action: ReactNode }) {
  return (
    <div className="group flex h-row items-center gap-2 rounded-ctl px-1 hover:bg-hover">
      <span className="text-muted">{icon}</span>
      <span className="w-16 shrink-0 text-dim">{label}</span>
      {value ? <TextValue className="flex-1">{value}</TextValue> : <span className="flex-1 truncate italic text-muted">{empty}</span>}
      <span className="opacity-60 group-hover:opacity-100">{action}</span>
    </div>
  );
}

const ic = "size-3.5";

export function PropertiesPanel() {
  const { propsTab, setPropsTab } = useWS();
  return (
    <div className="flex h-full flex-col">
      <div className="p-pad pb-gap">
        <TabBar
          value={propsTab}
          onChange={setPropsTab}
          items={[
            { id: "attributes", label: "Attributes" },
            { id: "material", label: "Material" },
            { id: "lighting", label: "Lighting" },
            { id: "render", label: "Render" },
          ]}
        />
      </div>
      <div className="gl-scroll flex flex-1 flex-col gap-gap px-pad pb-pad">
        {propsTab === "attributes" ? <Attributes /> : propsTab === "material" ? <Material /> : <Placeholder tab={propsTab} />}
      </div>
    </div>
  );
}

function Attributes() {
  return (
    <>
      <Section title="Groom">
        <Param label="Preview Density" init={1} />
        <Param label="Point Budget (k)" init={0} max={2000} step={10} hint="0: no limit" />
        <Row label="Strands"><TextValue>25,843</TextValue><span className="text-sm-ui text-muted">· 439k points</span></Row>
        <Bool label="Locked" />
      </Section>

      <Section title="Inputs">
        <Input icon={<Box className={ic} />} label="Scalp" value="483 vertices" action={<Tip label="Transfer to another mesh"><Button variant="ghost" size="iconSm"><Link2 className={ic} /></Button></Tip>} />
        <Input icon={<Box className={ic} />} label="Rest" empty="none" action={<Button variant="ghost" size="iconSm"><Plus className={ic} /></Button>} />
        <Input icon={<Spline className={ic} />} label="Guides" value="104 in 3 groups" action={<Tip label="Edit guides"><Button variant="ghost" size="iconSm"><Pencil className={ic} /></Button></Tip>} />
        <Input icon={<Shield className={ic} />} label="Collider" empty="none" action={<Tip label="Add a collider"><Button variant="ghost" size="iconSm"><Plus className={ic} /></Button></Tip>} />
        <Input
          icon={<FileText className={ic} />}
          label="File"
          value="manny_hair.groom"
          action={
            <span className="flex">
              <Tip label="Reload"><Button variant="ghost" size="iconSm"><RotateCw className={ic} /></Button></Tip>
              <Tip label="Save" shortcut="Ctrl+S"><Button variant="ghost" size="iconSm"><Save className={ic} /></Button></Tip>
            </span>
          }
        />
        <div className="h-1" />
        <Choice label="Scalp Subdivision" value="Off" options={["Off", "1", "2", "3"]} />
        <Bool label="Follow Scalp" />
        <Choice label="Scale Source" value="Surface Area" options={["Surface Area", "Bounding Box", "Fixed"]} />
        <Row label="Groom Scale"><TextValue>1.00×</TextValue><Button size="sm" variant="ghost" className="ml-auto">Set reference</Button></Row>
      </Section>

      <Section title="Controls" open={false}>
        <Param label="Global Length" init={1} max={2} />
      </Section>

      <Section title="Hair Color">
        <Choice label="Mode" value="Natural" options={["Natural", "Dyed", "Map", "Constant"]} />
        <Param label="Melanin" init={0.7} />
        <Param label="Redness" init={0.15} />
        <Row label="Dye"><Swatch color="#FFFFFF" value="#FFFFFF" /></Row>
        <Row label="Result"><Swatch color="#040100" value="#040100" /></Row>
        <Param label="Greys" init={0.9} />
        <Section title="Randomness" open={false}>
          <Param label="Melanin Random" init={0.08} />
          <Param label="Hue Random" init={0} />
          <Param label="Saturation Random" init={0} />
        </Section>
      </Section>

      <Section title="Display" open={false}>
        <Bool label="Display Hair" init />
        <Bool label="Display Guides" init />
        <Bool label="Display Flow" />
        <Choice label="Strand Color" value="Material" options={["Material", "Guide Group", "Clump", "Map", "Random"]} />
      </Section>
      <Section title="Sculpting" open={false}>
        <Param label="Sculpt Density" init={0.25} />
        <Param label="Soft Select Radius" init={0.1} />
      </Section>
      <Section title="Game Cards" open={false}>
        <Bool label="Display Cards" />
        <Param label="Card Density" init={0.5} />
      </Section>
      <Section title="Rendering" open={false}>
        <Choice label="Render Mode" value="Curves" options={["Curves", "Cards", "Both"]} />
      </Section>
    </>
  );
}

function Material() {
  return (
    <>
      <Section title="Shading">
        <Choice label="Shader" value="Hair (Arnold)" options={["Hair (Arnold)", "Hair (V-Ray)", "Flat"]} />
        <Param label="Specular" init={1} max={2} />
        <Param label="Roughness" init={0.2} />
        <Param label="Shift" init={3} min={-10} max={10} step={0.1} suffix="°" />
        <Param label="Transmission" init={0} />
      </Section>
      <Section title="Width">
        <Param label="Root Width" init={0.06} max={0.2} step={0.001} />
        <Param label="Tip Width" init={0.01} max={0.2} step={0.001} />
      </Section>
    </>
  );
}

function Placeholder({ tab }: { tab: string }) {
  return <p className="py-6 text-center text-sm-ui text-muted">{tab[0].toUpperCase() + tab.slice(1)} settings go here.</p>;
}
