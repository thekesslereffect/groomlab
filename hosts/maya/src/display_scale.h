// SPDX-License-Identifier: Apache-2.0
// Device pixels per point on the display under the cursor (2 on a Retina display). Maya's 3D view
// counts device pixels on macOS (M3dView::portWidth), while a person (and Qt, the app and Blender)
// counts points: the brush's radius, in points, is this many of the view's pixels. 1 elsewhere,
// where the view's pixels are the screen's.
#pragma once

namespace groomlab::maya {

#ifdef __APPLE__
float displayPixelScale();  // display_scale.cpp (CoreGraphics)
#else
inline float displayPixelScale() { return 1.0f; }
#endif

}  // namespace groomlab::maya
