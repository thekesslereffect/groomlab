#version 440
// SPDX-License-Identifier: Apache-2.0

// Shaded surfaces in per-vertex colors (Show Tubes: a Braid node's section tubes, each section its
// own color): surface.vert with a color stream.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 color;

layout(location = 0) out vec3 vPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 itemColor;
    vec4 highlight;
    vec4 camera;
    vec4 light;
};

void main() {
    vPosition = position;
    vNormal = normal;
    vColor = color;
    gl_Position = mvp * vec4(position, 1.0);
}
