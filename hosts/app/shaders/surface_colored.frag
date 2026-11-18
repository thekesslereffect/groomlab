#version 440
// SPDX-License-Identifier: Apache-2.0

// surface.frag's lighting (a key light, a dim fill from the other side, an ambient floor, both
// sides lit) on per-vertex colors: linear RGB as the core gives them (the clump colors' palette),
// encoded to sRGB for the window's target as flat.frag does (camera.w 1), left linear in the
// lookdev path's HDR scene (camera.w 0: its tone map encodes).

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColor;

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
    vec3 n = normalize(vNormal);
    vec3 v = normalize(camera.xyz - vPosition);
    if (dot(n, v) < 0.0) n = -n;
    vec3 l = normalize(light.xyz);
    float key = max(0.0, dot(n, l));
    float fill = max(0.0, dot(n, -l)) * 0.25;
    float rim = pow(1.0 - max(0.0, dot(n, v)), 3.0) * 0.15;
    vec3 lit = vColor.rgb * color.rgb * (light.w + (1.0 - light.w) * (key + fill)) + vec3(rim);
    fragColor = vec4(camera.w > 0.5 ? toSrgb(lit) : lit, 1.0);
}
