#version 440
// SPDX-License-Identifier: Apache-2.0

// A texture map on the scalp (the Paint view): the scalp lifted a hair off its surface, its uvs
// reading the map's image (rows bottom-up as uploaded: v 0 the image's bottom row, as the scalp's
// uvs have it), as Maya's viewport draws it.

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec2 vUv;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    vec4 highlight;
    vec4 camera;
    vec4 light;
};

void main() {
    vUv = uv;
    gl_Position = mvp * vec4(position, 1.0);
}
