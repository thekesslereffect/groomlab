#version 440
// SPDX-License-Identifier: Apache-2.0

// The backdrop behind the groom, in linear HDR before the hair: a solid color, a vertical
// gradient of it, or the environment itself (dimmed) seen along the view ray.

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Frame {
    mat4 domViewProj[5];
    vec4 domAxis[5];
    vec4 domParams;
    vec4 domParams2;
    vec4 eye;
    vec4 mat0;
    vec4 dye;
    vec4 mat1;
    vec4 specR;
    vec4 specTRT;
    vec4 specTT;
    vec4 lightDir[3];
    vec4 lightColor[3];
    vec4 sh[9];
    vec4 envDir[8];
    vec4 envColor[8];
    vec4 scalp;
    vec4 env;    // x: intensity, y: rotation (radians), z: background mode, w: background intensity
    vec4 bg;     // rgb the solid color
    vec4 tune;
    vec4 shadowParams;
    mat4 shadowViewProj[3];
    vec4 shadowAxis[3];
    vec4 debug;
};

layout(std140, binding = 1) uniform ViewBlock {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 view;
    vec4 pixel;
    vec4 axis;
    vec4 dom;
};

layout(binding = 9) uniform sampler2D environmentMap;

const float PI = 3.14159265359;

void main() {
    float mode = env.z;
    if (mode < 0.5) {
        fragColor = vec4(bg.rgb, 1.0);
        return;
    }
    // The view ray through this pixel (vUv addresses the frame as rendered: its y is the
    // target's row, so the clip-space y is recovered through the same flip).
    float ndcY = dom.w > 0.5 ? 1.0 - vUv.y : vUv.y;
    vec4 clip = vec4(vUv.x * 2.0 - 1.0, ndcY * 2.0 - 1.0, 1.0, 1.0);
    vec4 far = invViewProj * clip;
    vec3 dir = normalize(far.xyz / far.w - view.xyz);
    if (mode < 1.5) {
        float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
        fragColor = vec4(mix(bg.rgb * 0.75, bg.rgb * 1.25, t), 1.0);
        return;
    }
    float c = cos(env.y), s = sin(env.y);
    vec3 d = vec3(c * dir.x - s * dir.z, dir.y, s * dir.x + c * dir.z);
    float u = atan(d.x, -d.z) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    fragColor = vec4(texture(environmentMap, vec2(u, v)).rgb * env.w, 1.0);
}
