// SPDX-License-Identifier: Apache-2.0
// The hair's points as lookdev_renderer.cpp's buildHairBuffers lays them out: one RGBA32F texel a
// point in pointsTexture (x, y, z, width), and each point's strand in pointStrandsTexture (one
// float texel a point, exact below 2^24 strands: written only when the strands' layout changes,
// so a change of the points writes half what it did with the strand and the tangent beside each).
// The tangent is worked out here from the points either side. The includer declares pointsTexture
// and fetch() first.

layout(binding = 40) uniform sampler2D pointStrandsTexture;

uint hairStrandOf(uint point) {
    return uint(fetch(pointStrandsTexture, point).r + 0.5);
}

// The tangent at `point`, of a strand of `count` points from `first`: the central difference,
// one-sided at the ends, up for a lone point (the rule the CPU's buffers and the GPU preview's
// kernel had, written per point before).
vec3 hairTangent(uint point, uint first, uint count) {
    uint prev = point > first ? point - 1u : point;
    uint next = point + 1u < first + count ? point + 1u : point;
    vec3 d = fetch(pointsTexture, next).xyz - fetch(pointsTexture, prev).xyz;
    float len = length(d);
    return len > 1e-12 ? d / len : vec3(0.0, 1.0, 0.0);
}
