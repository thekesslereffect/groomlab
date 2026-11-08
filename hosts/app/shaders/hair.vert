#version 440
// SPDX-License-Identifier: Apache-2.0
#extension GL_GOOGLE_include_directive : require

// Hair strands as camera-facing ribbons, pulled from textures: no vertex streams, the vertex
// index alone says which point and which side of it this vertex is (two vertices a point, a
// triangle strip a strand). The points sit in a float texture a texel each (position and width;
// the strand of each in a texture of its own, the tangent from the points either side:
// hair_points.glsl), the strands in another (pigment and random, color and point range): a
// texel fetch in the vertex stage runs on every backend, where a storage buffer
// would not (Direct3D 11 wants feature level 11.1 for one). Each point is expanded across the
// strand by half its width, facing the pass's viewer (the camera, or a light's orthographic
// view for its deep opacity map). A strand thinner than a pixel is widened to one (the
// minimum pixels) and its coverage says how much of that pixel it really covers: the fragment
// shader fades it by that, so thin hair neither aliases nor vanishes. Under HAIR_GOURAUD
// (hair_gouraud.vert.qsb, the interactive frame) the point's shaded color comes along too,
// from the texture the per-point shading pass wrote (hair_lookdev.frag's HAIR_POINT_SHADE).

layout(location = 0) out vec3 vPosition;   // world
layout(location = 1) out vec3 vTangent;    // along the strand, unit
layout(location = 2) out vec4 vAttr;       // x: side (-1..1 across the ribbon), y: u root to tip, z: coverage, w: strand random
layout(location = 3) out vec4 vPigment;    // melanin, redness (the strand's channels), the point's iridescence and wetness
layout(location = 4) out vec3 vColor;      // the point's RGB (colorR/G/B per point, else the strand's)
layout(location = 5) out vec3 vRootNormal; // the scalp's normal at the root: what the head hides
#ifdef HAIR_GOURAUD
layout(location = 6) out vec3 vShade;      // the point's shaded color (linear HDR)
layout(binding = 4) uniform sampler2D shadedTexture;  // one texel a point, 4096 wide
#endif

layout(std140, binding = 1) uniform ViewBlock {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 view;   // xyz: the eye (perspective), or the direction toward the viewer (ortho); w: 1 ortho
    vec4 pixel;  // x: world units a pixel spans at distance 1 (ortho: per pixel); y: min pixels; z: width scale; w: pass index
    vec4 axis;   // xyz: the light's forward axis; w: the near plane's offset along it (dom passes)
    vec4 dom;    // x: layer thickness; y: range (far - near); z: the scalp's hair count; w: flip y
};

// 4096 texels wide (lookdev_renderer.cpp's kHairTextureWidth), a texel a point, three a strand.
layout(binding = 2) uniform sampler2D pointsTexture;   // (x, y, z, width)
layout(binding = 3) uniform sampler2D strandsTexture;  // (melanin, redness, random, first point), (r, g, b, point count), (root normal, 0)
// The points' own colors and iridescence (r, g, b, iridescence), a texel a point, where the
// strands carry them (a tip color, Bands, FeatherColor's patterns); else a single texel:
// each strand's color then, no iridescence.
layout(binding = 9) uniform sampler2D pointColorsTexture;

// hair_bsdf.glsl for hairLookAlpha alone (its calibration's prototypes stay undefined here, unused).
#include "../../../engine/gpu/shaders/hair_bsdf.glsl"

vec4 fetch(sampler2D tex, uint texel) {
    return texelFetch(tex, ivec2(int(texel & 4095u), int(texel >> 12u)), 0);
}

#include "hair_points.glsl"

void main() {
    uint vid = uint(gl_VertexIndex);
    uint point = vid >> 1u;
    float side = (vid & 1u) == 0u ? -1.0 : 1.0;
    vec4 a = fetch(pointsTexture, point);
    vec3 p = a.xyz;
    uint s = hairStrandOf(point);
    float width = a.w * pixel.z;
    vec4 d0 = fetch(strandsTexture, s * 3u);
    vec4 d1 = fetch(strandsTexture, s * 3u + 1u);
    vec4 d2 = fetch(strandsTexture, s * 3u + 2u);
    vec3 t = hairTangent(point, uint(d0.w + 0.5), uint(d1.w + 0.5));
    float count = max(d1.w, 2.0);
    float u = (float(point) - d0.w) / (count - 1.0);

    bool ortho = view.w > 0.5;
    vec3 toViewer = ortho ? view.xyz : (view.xyz - p);
    float dist = ortho ? 1.0 : max(length(toViewer), 1e-6);
    toViewer /= ortho ? 1.0 : dist;
    vec3 across = cross(t, toViewer);
    float al = length(across);
    // A strand pointing at the viewer: any direction across it will do.
    across = al > 1e-5 ? across / al : normalize(cross(t, abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));

    float pixelWorld = pixel.x * dist;
    float halfWidth = 0.5 * width;
    float minHalf = 0.5 * pixel.y * pixelWorld;
    float coverage = 1.0;
    if (halfWidth < minHalf) {
        coverage = halfWidth / max(minHalf, 1e-12);
        halfWidth = minHalf;
    }
    vec3 wp = p + across * (side * halfWidth);
    gl_Position = viewProj * vec4(wp, 1.0);
    vPosition = wp;
    vTangent = t;
    vAttr = vec4(side, u, coverage, d0.z);
    vec4 own = textureSize(pointColorsTexture, 0).x == 4096 ? fetch(pointColorsTexture, point) : vec4(d1.rgb, 0.0);
    vec2 look = hairLookAlpha(own.a, 0.0);  // the iridescence and wetness, unpacked per point
    vPigment = vec4(d0.x, d0.y, look.x, look.y);
    vColor = own.rgb;
    vRootNormal = d2.xyz;
#ifdef HAIR_GOURAUD
    vShade = fetch(shadedTexture, point).rgb;
#endif
}
