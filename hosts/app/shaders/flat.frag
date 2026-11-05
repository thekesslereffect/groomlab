#version 440
// SPDX-License-Identifier: Apache-2.0

// Flat colors (the guides, the dots, the tools' overlays): linear RGB, as the display theme and
// the core give them, encoded to sRGB for the window's 8-bit target (tonemap.frag's curve), so a
// theme's hex color shows as that hex.

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    vec4 highlight;
    vec4 camera;
    vec4 light;
};

vec3 toSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec4 c = vColor * color;
    fragColor = vec4(toSrgb(c.rgb), c.a);
}
