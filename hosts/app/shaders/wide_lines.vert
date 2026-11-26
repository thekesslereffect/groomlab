#version 440
// SPDX-License-Identifier: Apache-2.0

// Lines wider than a pixel (the guides' Width, the selected guides', the part lines): each segment a
// quad facing the camera, its four vertices at the segment's ends carrying the other end, a side
// (+1 or -1) and the half width in pixels, moved across the segment as it is seen on screen, so the
// width holds at any depth and angle (a line's own width is a pixel on Direct3D and Metal).

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 other;
layout(location = 2) in vec2 side;  // x: +1 or -1 across the segment, y: half the width in pixels
layout(location = 3) in vec4 color;

layout(location = 0) out vec4 vColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 itemColor;
    vec4 highlight;
    vec4 camera;
    vec4 light;
    vec4 viewport;  // the target's size in pixels (x, y)
};

void main() {
    vColor = color;
    vec4 clip = mvp * vec4(position, 1.0);
    vec4 far = mvp * vec4(other, 1.0);
    // The segment on screen, in pixels (each end over its own w; an end behind the eye: as is).
    vec2 a = clip.xy / max(abs(clip.w), 1e-6) * viewport.xy * 0.5;
    vec2 b = far.xy / max(abs(far.w), 1e-6) * viewport.xy * 0.5;
    vec2 along = b - a;
    float len = length(along);
    vec2 dir = len > 1e-4 ? along / len : vec2(1.0, 0.0);
    vec2 across = vec2(-dir.y, dir.x) * side.x * side.y;
    clip.xy += across * 2.0 / viewport.xy * clip.w;
    gl_Position = clip;
}
