// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's shared declarations (hair_raster.comp, hair_shade.comp, hair_voxelize.comp):
// the Vis block (lookdev_renderer.cpp's VisUniforms), the strands' textures as hair.vert reads them,
// and the per-pixel buffer.
//
// The per-pixel buffer holds four words a pixel, in the drawn target's memory order (rows as the
// texture stores them, the strides the full target's):
//   0: the visible fragment's key: its view depth's bits (shifted right by one), with the top bit
//      set when its hashed coverage did not keep it (the front fragment then stands in only where
//      no strand's fragment was kept); ~0: no hair;
//   1: the pixel's optical depth, sum over every fragment of -ln(1 - coverage), fixed point
//      (kTauScale): the hair's alpha is 1 - exp(-tau), order independent (Beer-Lambert);
//   2: the visible fragment's segment (its first point's index);
//   3: where on it, packUnorm2x16(the segment's parameter, the pixel's side across the strand).

layout(std140, binding = 1) uniform Vis {
    mat4 viewProj;   // the camera's (jittered as the frame is)
    vec4 eyeFrame;   // xyz: the eye; w: the frame's hash seed (0: the interactive frames' fixed pattern)
    vec4 target;     // x, y: the drawn size (pixels); z: the buffer's row stride (pixels, the full target's width); w: its rows
    vec4 params;     // x: pixels a scene unit spans at clip w 1; y: the width scale; z: the hair's opacity; w: 1 the rows run top down in memory
    vec4 params2;    // x: 1 the LOD buffer says what each strand stands for; y: 1 the surfaces' depth is tested; z: 1 NDC's y points up; w: the pass (0 depth and coverage, 1 the visible ids)
    uvec4 counts;    // x: points; y: threads a row of the dispatch; z: strands; w: unused
    vec4 voxel;      // xyz: unused; w: unused (hair_voxelize.comp's grid is the Frame's)
};

// 4096 texels wide (lookdev_renderer.cpp's kHairTextureWidth), a texel a point, three a strand.
layout(binding = 2) uniform sampler2D pointsTexture;   // (x, y, z, width): hair_points.glsl
layout(binding = 3) uniform sampler2D strandsTexture;  // (melanin, redness, random, first point), (r, g, b, point count), (root normal, 0)

layout(std430, binding = 4) buffer Pixels {
    uint pix[];
};

const float kTauScale = 65536.0;
const uint kNoHair = 0xFFFFFFFFu;

vec4 fetch(sampler2D tex, uint texel) {
    return texelFetch(tex, ivec2(int(texel & 4095u), int(texel >> 12u)), 0);
}

#include "hair_points.glsl"
