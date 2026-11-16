// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Tube tool's root overlay (RootTubeTool::overlay, the core's) drawn in a Maya viewport: each
// root's fill see-through, its edge along the scalp, its CVs and its middle as discs facing the
// camera, a drawn path in the air. Layered a hair toward the camera (the scalp, the fills, the
// edges, the dots) so none fights the scalp's depth.

#include <vector>

#include <maya/MFrameContext.h>
#include <maya/MUIDrawManager.h>

#include "groomlab/root_tube_tool.h"

namespace groomlab::maya {

// `pixelScale`: the screen's pixels per point (a high-DPI screen's dots as big to the eye).
void drawRootOverlay(MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& context,
                     const std::vector<RootTubeTool::Item>& items, float pixelScale);

}  // namespace groomlab::maya
