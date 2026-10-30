// SPDX-License-Identifier: Apache-2.0
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { OptionsProvider } from "./lab/store";
import "./index.css";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <OptionsProvider>
      <App />
    </OptionsProvider>
  </StrictMode>,
);
