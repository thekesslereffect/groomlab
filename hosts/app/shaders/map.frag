#version 440
// SPDX-License-Identifier: Apache-2.0

// The map's image, opaque (displayPixels made it as the map shows: gray, its color, or a channel).

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    vec4 highlight;
    vec4 camera;
    vec4 light;
};

layout(binding = 1) uniform sampler2D image;

void main() {
    fragColor = vec4(texture(image, vUv).rgb, 1.0);
}
