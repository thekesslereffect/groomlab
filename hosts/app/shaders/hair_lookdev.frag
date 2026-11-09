#version 440
// SPDX-License-Identifier: Apache-2.0
#extension GL_GOOGLE_include_directive : require

// The legacy hair path's shading (LookdevRenderer::HairPath::Legacy: the strands as ribbons through
// the raster pipeline, 4x MSAA): hair_shading.glsl's lobes per fragment, with the number of hairs
// in each light's way read from its deep opacity map and the hair's own ambient occlusion from the
// sky's. Linear HDR out, premultiplied by the strand's coverage so thin hair fades instead of
// aliasing; in refine mode the coverage is a stochastic discard instead, which the accumulation
// turns into exact order-independent transparency.
//
// Under HAIR_POINT_SHADE (hair_point_shade.frag.qsb) the same shading runs once per point
// instead of per fragment: a pass over a texture one texel a point (lookdev_renderer.cpp's
// shaded points) reads the point from hair.vert's textures, shades it as the ribbon's center
// would be (side 0, full coverage) and writes the color, which hair_gouraud.vert then fetches
// for the interactive frame's ribbons. The interactive frame's cost is then the points', not
// the overlapping fragments'.

#ifdef HAIR_POINT_SHADE
layout(binding = 2) uniform sampler2D pointsTexture;   // (x, y, z, width): hair_points.glsl
layout(binding = 3) uniform sampler2D strandsTexture;  // (melanin, redness, random, first point), (r, g, b, point count), (root normal, 0)
layout(binding = 9) uniform sampler2D pointColorsTexture;  // (r, g, b, iridescence) a point, or one texel (hair.vert)
vec3 vPosition;
vec3 vTangent;
vec4 vAttr;
vec4 vPigment;
vec3 vColor;
vec3 vRootNormal;
#else
layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vTangent;
layout(location = 2) in vec4 vAttr;     // side, u, coverage, random
layout(location = 3) in vec4 vPigment;  // melanin, redness, iridescence, wetness
layout(location = 4) in vec3 vColor;    // colorR/G/B (the point's)
layout(location = 5) in vec3 vRootNormal;
#endif

layout(location = 0) out vec4 fragColor;

#include "../../../engine/gpu/shaders/hair_bsdf.glsl"
#include "../../../engine/gpu/shaders/line_coverage.glsl"
#include "hair_shading.glsl"

layout(std140, binding = 1) uniform ViewBlock {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 view;
    vec4 pixel;  // w: the pass: 0 every strand (stochastic frames), 5 the strands at least a pixel wide, 6 the thinner ones
    vec4 axis;
    vec4 dom;    // y: the point count (HAIR_POINT_SHADE)
};

#ifdef HAIR_POINT_SHADE
vec4 fetch(sampler2D tex, uint texel) {
    return texelFetch(tex, ivec2(int(texel & 4095u), int(texel >> 12u)), 0);
}

#include "hair_points.glsl"
#endif

// The deep opacity maps (lookdev_renderer.cpp's kDom*): the three lights', the sky's, the camera's.
// Separate textures read through two samplers: Metal allows 16 samplers a stage, and the
// HAIR_POINT_SHADE pass would need 17 with a sampler each.
layout(binding = 5) uniform texture2D domDepth0;
layout(binding = 6) uniform texture2D domLayers0;
layout(binding = 7) uniform texture2D domDepth1;
layout(binding = 8) uniform texture2D domLayers1;
layout(binding = 14) uniform texture2D domDepth2;
layout(binding = 15) uniform texture2D domLayers2;
layout(binding = 16) uniform texture2D domDepth3;
layout(binding = 17) uniform texture2D domLayers3;
layout(binding = 12) uniform texture2D domDepth4;
layout(binding = 13) uniform texture2D domLayers4;
layout(binding = 10) uniform sampler domNearest;  // the depths
layout(binding = 11) uniform sampler domLinear;   // the layers
const int kDomSky = 3, kDomCamera = 4;
const float kPassThick = 5.0, kPassThin = 6.0;

float domFront(int which, vec2 uv) {
    if (which == 0) return texture(sampler2D(domDepth0, domNearest), uv).r;
    if (which == 1) return texture(sampler2D(domDepth1, domNearest), uv).r;
    if (which == 2) return texture(sampler2D(domDepth2, domNearest), uv).r;
    if (which == 3) return texture(sampler2D(domDepth3, domNearest), uv).r;
    return texture(sampler2D(domDepth4, domNearest), uv).r;
}
vec4 domLayers(int which, vec2 uv) {
    if (which == 0) return texture(sampler2D(domLayers0, domLinear), uv);
    if (which == 1) return texture(sampler2D(domLayers1, domLinear), uv);
    if (which == 2) return texture(sampler2D(domLayers2, domLinear), uv);
    if (which == 3) return texture(sampler2D(domLayers3, domLinear), uv);
    return texture(sampler2D(domLayers4, domLinear), uv);
}

// The number of hairs between the viewer of map `which` (a light's, the sky's, the camera's)
// and `pos` (coverage-weighted), from its deep opacity map: 0 in front of the first hair,
// the layers interpolated behind it.
float hairCount(int which, vec3 pos, vec2 jitter) {
    mat4 m = domViewProj[which];
    vec4 axis = domAxis[which];
    vec4 lp = m * vec4(pos, 1.0);
    vec2 uv = lp.xy / lp.w * 0.5 + 0.5;
    if (domParams2.y > 0.5) uv.y = 1.0 - uv.y;
    uv += jitter * domParams2.w;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
    float front = domFront(which, uv);
    bool camera = which == kDomCamera;
    float t = camera ? domParams.w : domParams.x;
    float d = dot(pos, axis.xyz) - axis.w - front - (camera ? 0.0 : domParams.z);
    if (d <= 0.0) return 0.0;
    vec4 layers = domLayers(which, uv);
    if (d <= t) return mix(0.0, layers.r, d / t);
    if (d <= 2.0 * t) return mix(layers.r, layers.g, (d - t) / t);
    if (d <= 4.0 * t) return mix(layers.g, layers.b, (d - 2.0 * t) / (2.0 * t));
    if (d <= 8.0 * t) return mix(layers.b, layers.a, (d - 4.0 * t) / (4.0 * t));
    return layers.a;
}

void main() {
#ifdef HAIR_POINT_SHADE
    // The texel is the point: its data as hair.vert reads it, the ribbon's center.
    uint point = uint(gl_FragCoord.y) * 4096u + uint(gl_FragCoord.x);
    if (float(point) >= dom.y) discard;
    vec4 a = fetch(pointsTexture, point);
    uint strand = hairStrandOf(point);
    vec4 d0 = fetch(strandsTexture, strand * 3u);
    vec4 d1 = fetch(strandsTexture, strand * 3u + 1u);
    vec4 d2 = fetch(strandsTexture, strand * 3u + 2u);
    vPosition = a.xyz;
    vTangent = hairTangent(point, uint(d0.w + 0.5), uint(d1.w + 0.5));
    vAttr = vec4(0.0, (float(point) - d0.w) / (max(d1.w, 2.0) - 1.0), 1.0, d0.z);
    vec4 own = textureSize(pointColorsTexture, 0).x == 4096 ? fetch(pointColorsTexture, point) : vec4(d1.rgb, 0.0);
    vec2 look = hairLookAlpha(own.a, 0.0);
    vPigment = vec4(d0.x, d0.y, look.x, look.y);
    vColor = own.rgb;
    vRootNormal = d2.xyz;
#endif
    float coverage = vAttr.z * specTT.w;
    bool stochastic = mod(specTRT.w, 2.0) >= 1.0;
    bool shadows = mod(floor(specTRT.w / 2.0), 2.0) >= 1.0;
    bool ao = mod(floor(specTRT.w / 4.0), 2.0) >= 1.0;
    // The random numbers: per pixel and frame for the shadow taps, and per strand too for the
    // coverage, so the strands over one pixel are kept independently of each other.
    uvec3 pixelSeed = uvec3(uvec2(gl_FragCoord.xy), uint(eye.w));
    vec3 rnd = random3(pixelSeed);
    float keep = random3(pixelSeed ^ uvec3(uint(vAttr.w * 1048576.0), uint(vAttr.y * 1048576.0), 0x9E37u)).x;
#ifdef HAIR_POINT_SHADE
    stochastic = false;
    coverage = 1.0;
    if (false) {
#else
    if (stochastic) {
#endif
        // Keep the fragment with the coverage's probability, opaque: the accumulation of such
        // frames is the exact transparency, with the strands in any order.
        if (keep >= coverage) discard;
        coverage = 1.0;
#ifdef HAIR_POINT_SHADE
    } else if (false) {
#else
    } else if (pixel.w > kPassThick - 0.5) {
#endif
        // The interactive frame draws the strands twice: those a pixel wide or more opaque with
        // their depth, then the thinner ones blended over everything without writing depth,
        // each weighed by the transmittance of the hair in front of it (the camera's deep
        // opacity map: the coverage summed is minus the log of it), so a strand behind the
        // front layers adds little and the order they blend in does not matter.
        bool thick = coverage >= 0.999;
        if ((pixel.w < kPassThick + 0.5) != thick) discard;
        if (!thick) {
            float through = exp(-hairCount(kDomCamera, vPosition, vec2(0.0)));
            if (through < 0.05) discard;  // buried: little of it would show, nothing to shade
            coverage *= through;
        }
    }

    HairPoint h;
    h.P = vPosition;
    h.T = normalize(vTangent);
    h.V = normalize(eye.xyz - vPosition);
    h.side = vAttr.x;
    h.pigment = vPigment;
    h.rgb = vColor;
    h.rootNormal = vRootNormal;
    h.random = vAttr.w;

    vec2 shadowJitter = stochastic ? (rnd.yz - 0.5) * 2.0 : vec2(0.0);
    // (The maps count coverage-weighted hairs, 2.55 times the hairs a ray crosses that hair_bsdf.glsl's
    // shadows take: the visibility path's volumes count those.)
    const float kDomToHairs = 0.3927;
    float skyCount = ao ? hairCount(kDomSky, vPosition, shadowJitter) * (kDomToHairs * 0.52) : 0.0;  // (the volumes' sky over their lights')
    float lightCount[3];
    bool lightMapped[3];
    for (int i = 0; i < 3; ++i) {
        lightMapped[i] = shadows && lightDir[i].w > 0.5 && lightColor[i].w > 0.5;
        lightCount[i] = lightMapped[i] ? hairCount(i, vPosition, shadowJitter) * kDomToHairs : 0.0;
    }
    vec3 result = hairShade(h, lightCount, lightMapped, skyCount, rnd.x * 6.2831853);

#ifdef HAIR_POINT_SHADE
    fragColor = vec4(result, 1.0);
#else
    fragColor = vec4(result * coverage, coverage);
#endif
}
