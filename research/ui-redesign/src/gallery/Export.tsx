// SPDX-License-Identifier: Apache-2.0
// The current design as Qt artifacts: a theme JSON for hosts/maya/themes and a reference QSS.
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { useLab } from "@/lab/store";
import { qss, themeJson } from "@/theme/qt";

function Block({ title, file, text }: { title: string; file: string; text: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <div className="flex min-h-0 flex-1 flex-col rounded-pnl border border-line bg-panel">
      <div className="flex items-center gap-2 border-b border-line px-pad py-1.5">
        <span className="font-semibold text-fg">{title}</span>
        <span className="num text-sm-ui text-muted">{file}</span>
        <div className="flex-1" />
        <Button size="sm" onClick={() => { navigator.clipboard?.writeText(text); setCopied(true); setTimeout(() => setCopied(false), 1200); }}>
          {copied ? "Copied" : "Copy"}
        </Button>
        <Button
          size="sm"
          onClick={() => {
            const a = document.createElement("a");
            a.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
            a.download = file.split("/").pop()!;
            a.click();
          }}
        >
          Download
        </Button>
      </div>
      <pre className="gl-scroll m-0 flex-1 select-text whitespace-pre-wrap p-pad font-['JetBrains_Mono',monospace] text-[11px] leading-relaxed text-dim">{text}</pre>
    </div>
  );
}

export function Export() {
  const { options } = useLab();
  const theme = themeJson(options);
  const slug = theme.name.toLowerCase().replace(/\s+/g, "_");
  return (
    <div className="gl-ui flex h-full gap-3 bg-background p-4">
      <Block title="Theme" file={`hosts/maya/themes/${slug}.json`} text={JSON.stringify(theme, null, 2)} />
      <Block title="Reference QSS" file={`${slug}.qss`} text={qss(options)} />
    </div>
  );
}
