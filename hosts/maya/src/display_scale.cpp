// SPDX-License-Identifier: Apache-2.0
// macOS only (display_scale.h): CoreGraphics' headers kept to this file (MacTypes' Point, Rect...).

#include "display_scale.h"

#include <CoreGraphics/CoreGraphics.h>

namespace groomlab::maya {

float displayPixelScale() {
    CGPoint at = CGPointZero;
    if (CGEventRef event = CGEventCreate(nullptr)) {  // (where the cursor is, in global points)
        at = CGEventGetLocation(event);
        CFRelease(event);
    }
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(at, 1, &display, &count) != kCGErrorSuccess || count == 0) display = CGMainDisplayID();
    float scale = 1.0f;
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display)) {
        const size_t points = CGDisplayModeGetWidth(mode), pixels = CGDisplayModeGetPixelWidth(mode);
        if (points > 0 && pixels >= points) scale = static_cast<float>(pixels) / static_cast<float>(points);
        CGDisplayModeRelease(mode);
    }
    return scale;
}

}  // namespace groomlab::maya
