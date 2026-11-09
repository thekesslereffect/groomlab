#version 440
// SPDX-License-Identifier: Apache-2.0

// Surfaces under the lookdev lights: the scalp, props (a scanned head) and eyes, in linear
// HDR, lit by the same lights, environment and deep opacity maps as the hair, shadowed by
// the other surfaces through a filtered shadow map per light. Three kinds of material (the
// Material block's flags.x):
//
//   Default: metal/roughness PBR. GGX normal distribution with Smith's height-correlated
//   visibility (Heitz 2014, in the form of Lagarde and de Rousiers 2014) and Schlick's Fresnel,
//   the lobe's lost multiple scattering put back for the lights after Turquin 2019 ("Practical
//   multiple scattering compensation for microfacet models") and for the environment after
//   Fdez-Aguera 2019 ("A Multiple-Scattering Microfacet Model for Real-Time Image-based
//   Lighting"); the environment through Karis 2013's split sum (the prefiltered map and the
//   environment BRDF table); Lambert's diffuse weighed by what the specular lobe leaves (one
//   minus its directional albedo from the same table: Fdez-Aguera's energy split, which stays
//   sane at grazing angles where Schlick's Fresnel of the macro normal alone would go to 1 and
//   black out the rim); the hair's shadows and ambient occlusion from the deep opacity maps,
//   the mesh's own baked occlusion, specular occlusion from it after Lagarde 2014.
//
//   Skin: light enters, scatters and leaves elsewhere (skin is not Lambertian). The
//   diffusion profile is Burley's normalized diffusion (Christensen and Burley 2015), with a
//   diffuse mean free path per channel (red the longest). Two ways to apply it:
//     - Screen space (Jimenez, Zsolnai, Jarabo and Gutierrez 2015, "Separable Subsurface
//       Scattering"): this shader's SSS_PREPASS variant writes the diffuse irradiance (the
//       lights' and the environment's diffuse through the shadows, the light transmitted
//       through thin parts) with part of the albedo (pre-scatter texturing) and the view
//       depth into their own targets; sss_blur.frag convolves it with the profile along two
//       screen axes, the kernel scaled by the material's radius over the pixel's depth and
//       following the surface across depth; the main variant reads it back at its pixel,
//       applies the rest of the albedo (post-scatter texturing) and adds the specular, which
//       keeps every pore while the diffuse goes soft.
//     - Pre-integrated (Penner 2011, "Pre-Integrated Skin Shading"): the profile integrated
//       around spheres, looked up by n.l and the curvature, so the terminator wraps and
//       reddens without a blur; the diffuse normal is the geometry's blended toward the
//       mapped one per channel (Penner's normal blur). The fallback when the blur cannot run.
//   Both: two GGX specular lobes (a tight one and a wide one over the oily surface, as d'Eon
//   2007 and Jimenez 2015 do), F0 0.028 (d'Eon's measured skin), the cavity (specular) map
//   occluding them; light through thin parts (ears, nostrils) from the thickness the shadow
//   map gives, through the profile's transmittance (Jimenez 2010, "Real-Time Realistic Skin
//   Translucency": the thickness read off the shadow map), which also softens a thin shadow's
//   edge; a micro-detail normal for pores.
//
//   Eye: Jimenez 2012's "Next-Generation Character Rendering" (GDC 2013) eye: the view ray
//   refracted at the cornea (IOR 1.376) and intersected with the iris plane behind it (the
//   iris moves with the view: parallax), a procedural iris of radial fibers with a limbal
//   ring, a pupil, the iris lit as a shallow bowl by the refracted light (the brightening on
//   the side away from the light that the cornea's focusing gives), a sclera with veins, the
//   lids' shadow from the baked occlusion, a wet band where the lids meet the eye (the tear
//   line) and the tear film's mirror-like specular over everything.
//
// Shadows from the surfaces: a distance map per light fit to the surfaces' bounds (the hair
// has its own deep opacity maps), read with a normal offset (Holbert 2011) and a slope-scaled
// bias, filtered over a rotated disk of taps (percentage-closer filtering, Reeves et al.
// 1987), the rotation varying per pixel and per accumulated frame so the filter converges to
// a soft disk.
//
// Debug views (the Frame block's debug.x, GROOMLAB_APP_DEBUG_VIEW): 1 diffuse alone, 2 specular
// alone, 3 the diffuse weight (one minus the specular albedo), 4 occlusion, 5 the first
// light's shadow, 6 the shading normal, 7 the geometric normal, 8 the transmittance through
// the thickness toward the first light, 9 the blurred irradiance, 10 the albedo, 11 the
// blur's width where the HDR pass found it.

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv;
layout(location = 4) in vec2 vExtra;  // occlusion, curvature (per scene unit)

#ifdef SSS_PREPASS
layout(location = 0) out vec4 fragIrradiance;  // rgb: the diffuse light (times albedo^texturing); a: the blur's width (0: none)
layout(location = 1) out float fragDepth;      // the view depth, along the camera's axis
#else
layout(location = 0) out vec4 fragColor;
#endif

layout(std140, binding = 0) uniform Frame {
    mat4 domViewProj[5];
    vec4 domAxis[5];
    vec4 domParams;    // x: layer thickness, y: shadow density, z: depth bias, w: the camera map's layer thickness
    vec4 domParams2;   // x: a map's texel in world units, y: flip y, z: ambient occlusion strength, w: texel size
    vec4 eye;          // xyz; w: frame index
    vec4 mat0;
    vec4 dye;
    vec4 mat1;
    vec4 specR;
    vec4 specTRT;      // w: flags (1 stochastic, 2 shadows, 4 ambient occlusion)
    vec4 specTT;
    vec4 lightDir[3];  // xyz toward the light; w: 1 when it casts a map
    vec4 lightColor[3];
    vec4 sh[9];
    vec4 envDir[8];    // xyz; w: active
    vec4 envColor[8];
    vec4 scalp;
    vec4 env;          // x: intensity, y: rotation (radians), z: background mode, w: background intensity
    vec4 bg;
    vec4 tune;
    vec4 shadowParams; // x: a shadow map's texel in world units, y: the filter's radius in texels, z: the map's size, w: 1 when the maps hold the surfaces
    mat4 shadowViewProj[3];
    vec4 shadowAxis[3];
    vec4 debug;        // x: the debug view
    // The hair's voxel volumes (the visibility path's shadows, hair_shading.glsl): the grid's
    // corner (w: 1 when they hold the hair, in place of the deep opacity maps), one over its extent
    // (w: a voxel's size), yzw: toward the sky.
    vec4 voxelLo;
    vec4 voxelScale;
    vec4 voxelParams;
};

layout(std140, binding = 1) uniform ViewBlock {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 view;
    vec4 pixel;
    vec4 axis;         // xyz: the camera's forward; w: the eye's distance along it
    vec4 dom;
};

layout(std140, binding = 4) uniform Material {
    vec4 base;         // rgb base color (linear); w roughness
    vec4 params;       // metallic, specular (F0 scale), occlusion strength, normal strength
    vec4 flags;        // x kind (0 default, 1 skin, 2 eye); y texture bits; z detail scale; w detail strength
    vec4 skin;         // sss radius scale, roughness 2, lobe 2 weight, cavity strength
    vec4 skin2;        // transmission, sss strength, specular map gain, sss texturing (albedo before the blur)
    vec4 eyeCenter;    // xyz; w radius
    vec4 eyeForward;   // xyz; w iris radius (of the eye's)
    vec4 eyeUp;        // xyz; w pupil radius (of the iris')
    vec4 eyeParams;    // cornea ior, limbal width, tear line, occlusion strength
    vec4 irisColor;    // rgb; w iris detail
    vec4 scleraColor;  // rgb; w veins
    vec4 eyeParams2;   // wetness, unused
    vec4 skin3;        // sss mode (0 pre-integrated, 1 screen space), the mean free path rgb (scene units)
    vec4 cutout;       // x: the albedo alpha's cutoff (0: opaque), y: the albedo map's width in texels
};
const float kTexAlbedo = 1.0, kTexNormal = 2.0, kTexRoughness = 4.0, kTexMetallic = 8.0, kTexOcclusion = 16.0, kTexSpecular = 32.0, kTexDetail = 64.0;

// The deep opacity maps (lookdev_renderer.cpp's kDom*): the three lights', the sky's.
layout(binding = 5) uniform texture2D domDepth0;
layout(binding = 6) uniform texture2D domLayers0;
layout(binding = 7) uniform texture2D domDepth1;
layout(binding = 8) uniform texture2D domLayers1;
layout(binding = 14) uniform texture2D domDepth2;
layout(binding = 15) uniform texture2D domLayers2;
layout(binding = 16) uniform texture2D domDepth3;
layout(binding = 17) uniform texture2D domLayers3;
// The environment prefiltered per roughness (a mip level each), the environment BRDF table,
// the pre-integrated skin table, the material's maps.
layout(binding = 18) uniform texture2D envPrefiltered;
layout(binding = 19) uniform texture2D brdfLut;
layout(binding = 20) uniform texture2D skinLut;
layout(binding = 21) uniform texture2D albedoMap;
layout(binding = 22) uniform texture2D normalMap;
layout(binding = 23) uniform texture2D maskMap;   // r roughness, g metallic, b occlusion, a specular
layout(binding = 24) uniform texture2D detailMap;
// The surfaces' shadow maps (distance from each light) and the blurred irradiance.
layout(binding = 25) uniform texture2D shadowMap0;
layout(binding = 26) uniform texture2D shadowMap1;
layout(binding = 27) uniform texture2D shadowMap2;
layout(binding = 28) uniform texture2D sssBlurred;
// The hair's voxel volumes: the hairs toward each light and the sky (hair_volume.comp).
// (Every binding here is 29 or less: Metal's vertex buffer goes after the highest, at 30 at most.)
layout(binding = 9) uniform texture3D hairVolume0;
layout(binding = 10) uniform texture3D hairVolume1;
layout(binding = 11) uniform texture3D hairVolume2;
layout(binding = 12) uniform texture3D hairVolume3;
// The samplers the textures above are read through (separate from them: Direct3D 11 has 16
// sampler slots, which 19 combined samplers would exceed; QRhi pairs them on every backend).
layout(binding = 29) uniform sampler sNearest;
layout(binding = 2) uniform sampler sLinear;
layout(binding = 3) uniform sampler sMipRepeat;    // the material's maps: trilinear, tiling
layout(binding = 13) uniform sampler sMipClamp;    // the prefiltered environment: its levels the roughness, the longitude wrapping
const int kDomSky = 3;
const float kEnvLevels = 6.0;

const float PI = 3.14159265359;

float sq(float x) { return x * x; }
bool hasTex(float bit) { return mod(floor(flags.y / bit), 2.0) >= 1.0; }

// Cutout (cards: the bake's coverage in the albedo map's alpha), as Maya's card shader tests it
// (groom_draw_override_fragments.cpp's groomLabCardColor): the coverage kept as the texture shrinks (each
// mip level down averages thin strands with the gaps beside them, so a quarter more a level);
// magnified, cut at the cutoff (crisp edges); a texel or more to a pixel, at a threshold hashed
// per pixel (in uv), so a strand thinner than a pixel shows as often as it covers it instead of
// not at all. surface_depth.frag cuts the shadow maps the same way.
bool cutAway(vec2 uv) {
    if (cutout.x <= 0.0 || !hasTex(kTexAlbedo)) return false;
    float a = texture(sampler2D(albedoMap, sMipRepeat), uv).a;
    vec2 footprint = max(fwidth(uv), vec2(1e-7));
    float lod = max(log2(max(max(footprint.x, footprint.y) * cutout.y, 1e-6)), 0.0);
    vec2 cell = floor(uv / footprint);
    float noise = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    return a * (1.0 + 0.25 * lod) < mix(cutout.x, noise, clamp(lod, 0.0, 1.0));
}

uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 random3(uvec3 seed) { return vec3(pcg3d(seed)) * (1.0 / 4294967296.0); }

// Value noise (a hash per lattice point, smoothly interpolated) for the iris and the veins.
float hash21(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)) + 65536);
    return float(pcg3d(uvec3(q, 17u)).x) * (1.0 / 4294967296.0);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1.0, 0.0)), c = hash21(i + vec2(0.0, 1.0)), d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// ---- The deep opacity maps (the hair's shadows) ---------------------------------------------

float domFront(int which, vec2 uv) {
    if (which == 0) return texture(sampler2D(domDepth0, sNearest), uv).r;
    if (which == 1) return texture(sampler2D(domDepth1, sNearest), uv).r;
    if (which == 2) return texture(sampler2D(domDepth2, sNearest), uv).r;
    return texture(sampler2D(domDepth3, sNearest), uv).r;
}
vec4 domLayers(int which, vec2 uv) {
    if (which == 0) return texture(sampler2D(domLayers0, sLinear), uv);
    if (which == 1) return texture(sampler2D(domLayers1, sLinear), uv);
    if (which == 2) return texture(sampler2D(domLayers2, sLinear), uv);
    return texture(sampler2D(domLayers3, sLinear), uv);
}

// The map's texel for a point, and whether the point projects into the map.
bool domUv(int which, vec3 pos, vec2 jitter, out vec2 uv) {
    vec4 lp = domViewProj[which] * vec4(pos, 1.0);
    uv = lp.xy / lp.w * 0.5 + 0.5;
    if (domParams2.y > 0.5) uv.y = 1.0 - uv.y;
    uv += jitter * domParams2.w;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

// How many hairs are between the map's viewer and `pos`: 0 in front of the first hair, the
// layers interpolated behind it. A texel nothing was drawn into keeps the cleared 0 (the
// passes write at least a little more): nothing in the way.
float hairCount(int which, vec3 pos, vec2 jitter) {
    if (voxelLo.w > 0.5) {
        // The visibility path: the voxel volume's count, a voxel on toward the light.
        vec3 toward = which == kDomSky ? voxelParams.yzw : lightDir[which].xyz;
        // (The volumes count the hairs a ray crosses; the surfaces' shadows take 2.55 times that, as
        // fitted to Arnold when the volumes held it.)
        vec3 uvw = (pos + toward * voxelScale.w - voxelLo.xyz) * voxelScale.xyz;
        if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) return 0.0;
        float hairs = which == 0 ? textureLod(sampler3D(hairVolume0, sLinear), uvw, 0.0).r
                    : which == 1 ? textureLod(sampler3D(hairVolume1, sLinear), uvw, 0.0).r
                    : which == 2 ? textureLod(sampler3D(hairVolume2, sLinear), uvw, 0.0).r
                    : textureLod(sampler3D(hairVolume3, sLinear), uvw, 0.0).r;
        return hairs * 2.546;
    }
    vec2 uv;
    if (!domUv(which, pos, jitter, uv)) return 0.0;
    float front = domFront(which, uv);
    if (front <= 0.0) return 0.0;
    float d = dot(pos, domAxis[which].xyz) - domAxis[which].w - front - domParams.z;
    if (d <= 0.0) return 0.0;
    vec4 layers = domLayers(which, uv);
    float t = domParams.x;
    if (d <= t) return mix(0.0, layers.r, d / t);
    if (d <= 2.0 * t) return mix(layers.r, layers.g, (d - t) / t);
    if (d <= 4.0 * t) return mix(layers.g, layers.b, (d - 2.0 * t) / (2.0 * t));
    if (d <= 8.0 * t) return mix(layers.b, layers.a, (d - 4.0 * t) / (4.0 * t));
    return layers.a;
}

// ---- The surfaces' shadow maps -----------------------------------------------------------------

float shadowDepth(int which, vec2 uv) {
    if (which == 0) return texture(sampler2D(shadowMap0, sNearest), uv).r;
    if (which == 1) return texture(sampler2D(shadowMap1, sNearest), uv).r;
    return texture(sampler2D(shadowMap2, sNearest), uv).r;
}

bool shadowUv(int which, vec3 pos, out vec2 uv) {
    vec4 lp = shadowViewProj[which] * vec4(pos, 1.0);
    uv = lp.xy / lp.w * 0.5 + 0.5;
    if (domParams2.y > 0.5) uv.y = 1.0 - uv.y;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

// How much of light `which` the other surfaces let through to `pos` (0 shadowed, 1 lit),
// filtered. The point is read a little off the surface along its normal, more where the
// light grazes it (the normal offset of Holbert 2011, "Shadow Mapping: GPU-based Tips and
// Techniques"), and compared with a bias that grows with the slope the map's texel spans
// across the surface and with the filter's radius, so a surface never shadows itself with
// its own quantization and the terminator stays clean. The taps lie on a Vogel disk (the
// golden angle) of the filter's radius, rotated by `rotation`.
float surfaceShadow(int which, vec3 pos, vec3 Ng, float nDotL, float rotation) {
    if (shadowParams.w < 0.5) return 1.0;
    float texel = shadowParams.x, radius = shadowParams.y;
    float cosine = clamp(nDotL, 0.0, 1.0);
    float tanTheta = sqrt(1.0 - cosine * cosine) / max(cosine, 0.05);
    vec3 p = pos + Ng * (texel * 1.5 * (1.0 - cosine));
    vec2 uv;
    if (!shadowUv(which, p, uv)) return 1.0;
    float receiver = dot(p, shadowAxis[which].xyz) - shadowAxis[which].w;
    float bias = texel * (1.0 + (1.0 + radius) * min(tanTheta, 6.0));
    float lit = 0.0;
    const int taps = 12;
    float c = cos(rotation), s = sin(rotation);
    for (int k = 0; k < taps; ++k) {
        float r = sqrt((float(k) + 0.5) / float(taps)) * radius;
        float a = float(k) * 2.39996323;
        vec2 o = vec2(cos(a), sin(a)) * r;
        o = vec2(c * o.x - s * o.y, s * o.x + c * o.y) / shadowParams.z;
        float d = shadowDepth(which, uv + o);
        lit += (d <= 0.0 || receiver - bias <= d) ? 1.0 : 0.0;
    }
    // Where the light all but grazes the surface no bias follows its depth across a texel:
    // the map's say fades out over the last degrees (the surface gets little light there).
    return mix(1.0, lit / float(taps), smoothstep(0.0, 0.08, cosine));
}

// How deep `pos` sits behind the first surface light `which` meets, in scene units (the
// thickness the light crossed to reach it: what thin skin transmits by), the map's
// distance read bilinearly so the thickness is continuous across its texels (a nearest
// read stair-steps where the light grazes a surface) and lifted by what the slope spans in
// a texel there. Nothing in the map (a surface that casts nothing) or outside it: too thick
// to pass anything.
float thicknessToward(int which, vec3 pos, float nDotL) {
    if (shadowParams.w < 0.5) return 1e3;
    vec2 uv;
    if (!shadowUv(which, pos, uv)) return 1e3;
    float front = which == 0 ? texture(sampler2D(shadowMap0, sLinear), uv).r
                : which == 1 ? texture(sampler2D(shadowMap1, sLinear), uv).r : texture(sampler2D(shadowMap2, sLinear), uv).r;
    if (front <= 0.0) return 1e3;
    float cosine = abs(nDotL);
    float slope = sqrt(1.0 - cosine * cosine) / max(cosine, 0.05);
    return max(0.0, dot(pos, shadowAxis[which].xyz) - shadowAxis[which].w - front) + shadowParams.x * 0.5 * min(slope, 6.0);
}

// ---- The BRDF --------------------------------------------------------------------------------

float dGGX(float nDotH, float a) {
    float d = nDotH * nDotH * (a * a - 1.0) + 1.0;
    return a * a / (PI * d * d);
}
float vSmithGGX(float nDotV, float nDotL, float a) {
    float a2 = a * a;
    float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 fSchlick(vec3 f0, float c) { return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); }

// One GGX lobe for a light: D V F n.l, with the energy its single scattering loses at high
// roughness put back (Turquin: 1 + F0 (1 / E - 1), E the lobe's directional albedo from the
// split-sum table).
vec3 specularLobe(vec3 N, vec3 V, vec3 L, vec3 f0, float roughness, float nDotV) {
    float a = max(roughness * roughness, 1e-3);
    vec3 H = normalize(V + L);
    float nDotL = max(dot(N, L), 0.0), nDotH = max(dot(N, H), 0.0), vDotH = max(dot(V, H), 0.0);
    vec2 ab = texture(sampler2D(brdfLut, sLinear), vec2(nDotV, roughness)).rg;
    float ess = max(ab.x + ab.y, 1e-3);
    vec3 compensation = 1.0 + f0 * (1.0 / ess - 1.0);
    return dGGX(nDotH, a) * vSmithGGX(nDotV, nDotL, a) * fSchlick(f0, vDotH) * compensation * nDotL;
}

// The lobe's directional albedo at this view (the split-sum table's scale and bias on F0):
// what the specular takes of the light, which the diffuse does not get.
vec3 specularAlbedo(vec3 f0, float roughness, float nDotV) {
    vec2 ab = texture(sampler2D(brdfLut, sLinear), vec2(nDotV, roughness)).rg;
    return f0 * ab.x + ab.y;
}

vec3 irradianceSH(vec3 N) {
    vec3 irr = sh[0].rgb + sh[1].rgb * N.y + sh[2].rgb * N.z + sh[3].rgb * N.x + sh[4].rgb * (N.x * N.y) +
               sh[5].rgb * (N.y * N.z) + sh[6].rgb * (N.z * N.z) + sh[7].rgb * (N.x * N.z) + sh[8].rgb * (N.x * N.x - N.y * N.y);
    return max(irr, vec3(0.0)) * env.x;
}

// The environment's radiance around a direction for a lobe of `roughness` (the prefiltered
// map's level), with the environment's rotation.
vec3 envSpecular(vec3 R, float roughness) {
    float c = cos(env.y), s = sin(env.y);
    vec3 d = vec3(c * R.x - s * R.z, R.y, s * R.x + c * R.z);
    float u = atan(d.x, -d.z) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return textureLod(sampler2D(envPrefiltered, sMipClamp), vec2(u, v), roughness * (kEnvLevels - 1.0)).rgb * env.x;
}

// The environment's specular through the split sum, the multiple scattering added back
// (Fdez-Aguera's energy-conserving form).
vec3 envSpecularSplitSum(vec3 N, vec3 V, vec3 f0, float roughness, float nDotV) {
    vec3 R = reflect(-V, N);
    vec3 radiance = envSpecular(R, roughness);
    vec2 ab = texture(sampler2D(brdfLut, sLinear), vec2(nDotV, roughness)).rg;
    vec3 fssEss = f0 * ab.x + ab.y;
    float ess = ab.x + ab.y;
    float ems = 1.0 - ess;
    vec3 favg = f0 + (1.0 - f0) / 21.0;
    vec3 fms = fssEss * favg / (1.0 - ems * favg);
    return radiance * (fssEss + fms * ems);
}

// Specular occlusion from ambient occlusion (Lagarde and de Rousiers 2014).
float specularOcclusion(float nDotV, float ao, float roughness) {
    return clamp(pow(nDotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// Light through skin `thickness` scene units thick: diffusion theory's decay e^(-t/d) per
// channel (look.h's burleyTransmittance) at the material's mean free paths times its
// radius, 1 at no thickness.
vec3 skinTransmittance(float thickness) {
    vec3 d = max(skin3.yzw * max(skin.x, 0.01), vec3(1e-4));
    return exp(-thickness / d);
}

// Penner's pre-integrated diffuse: the profile around a sphere of this curvature (per mm).
vec3 skinDiffuse(float nDotL, float curvaturePerMm) {
    return texture(sampler2D(skinLut, sLinear), vec2(nDotL * 0.5 + 0.5, clamp(curvaturePerMm, 0.0, 1.0))).rgb;
}

// ---- Eyes ------------------------------------------------------------------------------------

const float kEyeIrisPlane = 0.84;  // the iris' plane ahead of the center, in radii (surface.h)

struct EyeShade {
    vec3 albedo;    // what the point reflects diffusely
    vec3 diffuseN;  // the normal the diffuse is lit by (the iris as a bowl, the sclera as is)
    bool cornea;    // the point is on the cornea: the light refracts too
    float darken;   // the limbal ring and the pupil
};

// The iris: fibers radiating from the pupil (noise stretched along the radius), a collarette
// ring, a limbal ring darkening toward the edge, the pupil black with a soft edge.
vec3 irisPattern(vec2 p, float r, float pupil, float limbal, float detail) {
    float angle = atan(p.y, p.x);
    float fibers = vnoise(vec2(angle * 14.0, r * 6.0)) * 0.6 + vnoise(vec2(angle * 40.0 + 7.0, r * 18.0)) * 0.4;
    float rings = 0.5 + 0.5 * sin(r * 28.0 + 3.0 * vnoise(vec2(angle * 3.0, 1.0)));
    float tone = mix(1.0, 0.55 + 0.9 * fibers + 0.25 * rings, detail);
    vec3 color = irisColor.rgb * tone;
    // The collarette: a brighter, warmer ring a third of the way out.
    color *= 1.0 + 0.35 * detail * exp(-sq((r - 0.45) / 0.08));
    // The limbal ring: the edge goes dark over `limbal` of the radius.
    color *= smoothstep(1.0, 1.0 - max(limbal, 0.01), r);
    // The pupil, its edge a little soft.
    color *= smoothstep(pupil, pupil * 1.12, r);
    return color;
}

EyeShade shadeEyePoint(vec3 P, vec3 N, vec3 V) {
    EyeShade e;
    float R = eyeCenter.w;
    vec3 F = normalize(eyeForward.xyz), U = normalize(eyeUp.xyz), S = normalize(cross(U, F));
    vec3 rel = (P - eyeCenter.xyz) / R;
    vec2 xy = vec2(dot(rel, S), dot(rel, U));
    float irisR = eyeForward.w;
    e.cornea = length(xy) < irisR && dot(rel, F) > 0.0;
    e.darken = 1.0;
    e.diffuseN = N;
    if (e.cornea) {
        // Into the eye: the refracted view ray meets the iris plane; the iris is read there.
        vec3 I = refract(-V, N, 1.0 / eyeParams.x);
        float denom = dot(I, F);
        float t = denom < -1e-4 ? (kEyeIrisPlane - dot(rel, F)) / denom : 0.0;
        vec3 hit = rel + I * max(t, 0.0);
        vec2 ixy = vec2(dot(hit, S), dot(hit, U)) / irisR;
        float r = length(ixy);
        if (r < 1.0) {
            e.albedo = irisPattern(ixy, r, eyeUp.w, eyeParams.y, irisColor.w);
            // The iris as a shallow bowl: its normal leans toward the center, so the side away
            // from the light (where the cornea focuses it) is the lit one.
            vec3 radial = normalize(S * ixy.x + U * ixy.y);
            e.diffuseN = normalize(F - radial * 0.45);
            e.darken = smoothstep(eyeUp.w, eyeUp.w * 1.12, r);
        } else {
            e.albedo = scleraColor.rgb;
        }
    } else {
        // The sclera: veins as thin red noise ridges, a little denser toward the back.
        float back = clamp((length(xy) - irisR) / (1.0 - irisR), 0.0, 1.0);
        float v1 = vnoise(xy * 18.0), v2 = vnoise(xy * 37.0 + 5.0);
        float ridge = 1.0 - abs(2.0 * v1 - 1.0);
        float veins = pow(ridge, 6.0) * (0.5 + 0.5 * v2) * scleraColor.w;
        vec3 sclera = scleraColor.rgb * mix(1.0, 0.85, back * 0.6);
        e.albedo = mix(sclera, vec3(0.6, 0.12, 0.1), clamp(veins, 0.0, 0.5));
    }
    return e;
}

// ---- Main ------------------------------------------------------------------------------------

// A normal that faces away from the viewer bent to the horizon (a hair toward the viewer):
// an interpolated normal at a silhouette, or a mapped one leaning over it, must not light
// as if it looked away, which blacks the rim out.
vec3 towardViewer(vec3 N, vec3 V) {
    float c = dot(N, V);
    return c < 0.001 ? normalize(N - V * (c - 0.001)) : N;
}

void main() {
    // The cutout first (the map's uv as below), its derivatives taken while the whole quad runs.
    if (cutAway(vec2(vUv.x, 1.0 - vUv.y))) discard;
    bool stochastic = mod(specTRT.w, 2.0) >= 1.0;
    bool shadows = mod(floor(specTRT.w / 2.0), 2.0) >= 1.0;
    bool ao = mod(floor(specTRT.w / 4.0), 2.0) >= 1.0;
    int kind = int(flags.x + 0.5);
    uvec3 pixelSeed = uvec3(uvec2(gl_FragCoord.xy), uint(eye.w));
    vec3 rnd = random3(pixelSeed);
    vec2 shadowJitter = stochastic ? (rnd.yz - 0.5) * 2.0 : vec2(0.0);
    float rotation = rnd.x * 2.0 * PI;

    vec3 V = normalize(eye.xyz - vPosition);
    // The face's own normal, toward the viewer (a visible face of a closed mesh faces it):
    // an interpolated normal on the other side belongs to a surface seen from inside, lit
    // as the outside is; one merely leaning past the horizon at a silhouette is bent back.
    vec3 Nface = cross(dFdx(vPosition), dFdy(vPosition));
    if (dot(Nface, V) < 0.0) Nface = -Nface;
    vec3 Ng = normalize(vNormal);
    if (dot(Ng, Nface) < 0.0) Ng = -Ng;
    Ng = towardViewer(Ng, V);
    vec3 T = normalize(vTangent.xyz - Ng * dot(Ng, vTangent.xyz));
    vec3 B = cross(Ng, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);

    // The maps. OBJ's v runs up from the image's bottom (as the core samples the scalp's
    // textures); the texture's rows run down from its top. The albedo is an sRGB texture:
    // the device decodes it before filtering (linear mips, linear interpolation); the data
    // maps are read as they are.
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    vec3 albedo = base.rgb;
    if (hasTex(kTexAlbedo)) albedo *= texture(sampler2D(albedoMap, sMipRepeat), uv).rgb;
    vec4 mask = texture(sampler2D(maskMap, sMipRepeat), uv);
    float roughness = base.w * (hasTex(kTexRoughness) ? mask.r : 1.0);
    float metallic = params.x * (hasTex(kTexMetallic) ? mask.g : 1.0);
    float occlusion = mix(1.0, (hasTex(kTexOcclusion) ? mask.b : 1.0) * vExtra.x, params.z);
    float cavity = hasTex(kTexSpecular) ? mix(1.0, clamp(mask.a * skin2.z, 0.0, 1.0), skin.w) : 1.0;
    // The shading normal: the map's slope scaled, the detail's slopes added (Barre-Brisebois
    // and Hill 2012's UDN blend), in the tangent frame.
    vec3 N = Ng;
    vec3 nt = vec3(0.0, 0.0, 1.0);
    if (hasTex(kTexNormal)) {
        nt = texture(sampler2D(normalMap, sMipRepeat), uv).xyz * 2.0 - 1.0;
        nt.xy *= params.w;
    }
    if (hasTex(kTexDetail) && flags.z > 0.0) {
        vec3 d = texture(sampler2D(detailMap, sMipRepeat), uv * flags.z).xyz * 2.0 - 1.0;
        nt.xy += d.xy * flags.w;
    }
    if (hasTex(kTexNormal) || hasTex(kTexDetail)) N = towardViewer(normalize(T * nt.x + B * nt.y + Ng * max(nt.z, 0.1)), V);
    float nDotV = max(dot(N, V), 1e-3);

    // The dielectric F0 as Burley scales it (0.5: 4 percent), metals taking their color.
    vec3 f0 = mix(vec3(0.08 * params.y), albedo, metallic);
    vec3 diffuseColor = albedo * (1.0 - metallic);
    float roughness2 = kind == 1 ? skin.y : roughness;
    float lobe2 = kind == 1 ? skin.z : 0.0;

    // Skin: screen-space scattering (the blur does the spreading: Lambert's irradiance goes
    // into the prepass) or pre-integrated (the profile by curvature, the diffuse reading a
    // blurrier normal per channel: the geometry's for red, toward the mapped one for blue).
    float sssMix = kind == 1 ? skin2.y : 0.0;
    bool screenSss = kind == 1 && skin3.x > 0.5 && sssMix > 0.0;
    vec3 Nr = screenSss ? N : normalize(mix(Ng, N, 0.25)), Ngn = screenSss ? N : normalize(mix(Ng, N, 0.6)), Nb = N;
    float curvaturePerMm = vExtra.y * 0.1 / max(skin.x, 0.05);  // per cm to per mm, over the profile's scale

    EyeShade eyeShade;
    if (kind == 2) {
        eyeShade = shadeEyePoint(vPosition, Ng, V);
        diffuseColor = eyeShade.albedo;
        N = Ng;  // the tear film is smooth
        nDotV = max(dot(N, V), 1e-3);
        f0 = vec3(0.08 * params.y);
        // The lids' shadow: the baked occlusion, raised to the strength (the lids and lashes
        // overhang more than rays from a bare socket say).
        occlusion = pow(max(vExtra.x, 1e-3), eyeParams.w);
    }

    // The hair's maps are read a texel or two off the surface along its normal, so a map's
    // own quantization of this very surface does not shadow it.
    vec3 lookup = vPosition + Ng * (domParams2.x * 2.0);
    float skyCount = ao ? hairCount(kDomSky, lookup, shadowJitter) * domParams2.z : 0.0;
    float hairAo = exp(-skyCount * 0.5);

    vec3 diffuse = vec3(0.0), specular = vec3(0.0), transmitted = vec3(0.0);
    float firstShadow = 1.0;
    vec3 firstThrough = vec3(0.0);
    // The lights: the three with their shadow maps. The environment comes whole through its
    // irradiance (the spherical harmonics) and the split sum below; the directional lights
    // that stand for it light the hair's lobes alone (here they would count it twice).
    for (int i = 0; i < 3; ++i) {
        bool envLight = false;
        if (!envLight && lightColor[i].w < 0.5) continue;
        if (envLight && envDir[i - 3].w < 0.5) continue;
        vec3 L = envLight ? envDir[i - 3].xyz : lightDir[i].xyz;
        vec3 color = envLight ? envColor[i - 3].rgb * env.x : lightColor[i].rgb;
        float shadow = 1.0;
        float thickness = 1e3;
        float nDotLg = dot(Ng, L);
        if (envLight) {
            shadow = exp(-skyCount * (0.5 + 0.5 * L.y) * 0.5) * occlusion;
        } else if (shadows && lightDir[i].w > 0.5) {
            // The hair in the way, then the other surfaces (the filtered map).
            shadow = exp(-hairCount(i, lookup, shadowJitter) * domParams.y);
            shadow *= surfaceShadow(i, vPosition, Ng, nDotLg, rotation);
            // The thickness toward the light, read a little inside the surface so a point on
            // the far side does not count its own quantization as thickness.
            thickness = thicknessToward(i, vPosition - Ng * shadowParams.x, nDotLg);
            if (i == 0) firstShadow = shadow;
        }
        float nDotL = dot(N, L);
        vec3 d;
        vec3 diffuseShadow = vec3(shadow);
        if (kind == 2) {
            // The iris under the refracted light, the sclera under the light as it comes.
            vec3 Ld = L;
            if (eyeShade.cornea) Ld = -refract(-L, Ng, 1.0 / eyeParams.x);
            d = vec3(max(dot(eyeShade.diffuseN, Ld), 0.0) / PI) * eyeShade.darken;
        } else if (kind == 1) {
            if (screenSss) {
                d = vec3(max(nDotL, 0.0) / PI);
            } else {
                // Per channel: the pre-integrated profile at each channel's normal and the curvature.
                vec3 pre = vec3(skinDiffuse(dot(Nr, L), curvaturePerMm).r, skinDiffuse(dot(Ngn, L), curvaturePerMm).g,
                                skinDiffuse(dot(Nb, L), curvaturePerMm).b);
                d = mix(vec3(max(nDotL, 0.0)), pre, sssMix) / PI;
            }
            if (!envLight && lightDir[i].w > 0.5 && shadows) {
                // The shadow's edge is softened by what the skin passes at this depth behind
                // the lit surface (Jimenez 2010 reads translucency off the shadow map this
                // way): the light past the terminator survives a thin shadow, red the deepest.
                vec3 through = skinTransmittance(thickness) * sssMix;
                diffuseShadow = max(diffuseShadow, through);
                if (i == 0) firstThrough = skinTransmittance(thickness);
                // Through the thin parts: light from behind, by the same profile (part of
                // the scattering: its strength scales this too).
                if (skin2.x > 0.0) {
                    vec3 t = skinTransmittance(thickness) * clamp(0.3 + dot(-Ng, L), 0.0, 1.0);
                    transmitted += color * t * (skin2.x * sssMix / PI);
                }
            }
        } else {
            d = vec3(max(nDotL, 0.0) / PI);
        }
        diffuse += color * d * diffuseShadow;
        if (nDotL > 0.0) {
            vec3 s = specularLobe(N, V, L, f0, roughness, nDotV);
            if (lobe2 > 0.0) s = mix(s, specularLobe(N, V, L, f0, roughness2, nDotV), lobe2);
            specular += color * s * shadow;
        }
    }
    // The environment's diffuse (its irradiance) and glossy reflection, through the hair's
    // occlusion and the mesh's own.
    float ambientAo = hairAo * occlusion;
    vec3 irr;
    if (kind == 1) {
        irr = vec3(irradianceSH(Nr).r, irradianceSH(Ngn).g, irradianceSH(Nb).b);
    } else if (kind == 2) {
        irr = irradianceSH(eyeShade.diffuseN) * eyeShade.darken;
    } else {
        irr = irradianceSH(N);
    }
    diffuse += irr * ambientAo / PI;
    float so = specularOcclusion(nDotV, ambientAo, roughness) * cavity;
    vec3 envSpec = envSpecularSplitSum(N, V, f0, roughness, nDotV);
    if (lobe2 > 0.0) envSpec = mix(envSpec, envSpecularSplitSum(N, V, f0, roughness2, nDotV), lobe2);
    specular = specular * cavity + envSpec * so;

    // What the specular lobes take at this view is not diffused: their directional albedo
    // (the split-sum table), which a rough dielectric keeps far below 1 even at grazing
    // angles, unlike Schlick's Fresnel of the macro normal alone.
    vec3 specAlbedo = specularAlbedo(f0, roughness, nDotV);
    if (lobe2 > 0.0) specAlbedo = mix(specAlbedo, specularAlbedo(f0, roughness2, nDotV), lobe2);
    vec3 kd = (1.0 - specAlbedo) * (1.0 - metallic);
    float texturing = clamp(skin2.w, 0.0, 1.0);

#ifdef SSS_PREPASS
    // The diffuse light (the transmitted light with it: it diffuses too) with the albedo's
    // pre-scatter share, and the blur's width (the radius scale times the strength), for
    // the skin the blur serves; the others leave the targets clear.
    vec3 irradiance = (diffuse + transmitted) * pow(max(diffuseColor, vec3(0.0)), vec3(texturing));
    float width = screenSss ? max(skin.x, 0.01) * sssMix : 0.0;
    fragIrradiance = width > 0.0 ? vec4(irradiance, width) : vec4(0.0);
    fragDepth = dot(vPosition, axis.xyz) - axis.w;
#else
    vec3 diffuseLit = diffuseColor * (diffuse + transmitted);
    vec4 blurred = vec4(0.0);
    if (screenSss) {
        blurred = texelFetch(sampler2D(sssBlurred, sNearest), ivec2(gl_FragCoord.xy), 0);
        if (blurred.a > 0.0) diffuseLit = blurred.rgb * pow(max(diffuseColor, vec3(0.0)), vec3(1.0 - texturing));
    }
    if (kind == 2) {
        // The tear film's reflection over everything, and the tear line: where the lids'
        // shadow starts, a wet band with a sharper highlight.
        float wet = eyeParams2.x;
        float band = eyeParams.z * smoothstep(0.2, 0.45, vExtra.x) * (1.0 - smoothstep(0.55, 0.8, vExtra.x));
        specular *= wet * (1.0 + 2.0 * band);
        diffuseLit *= 1.0 - 0.3 * band;
    }
    vec3 result = diffuseLit * kd + specular;
    int view = int(debug.x + 0.5);
    if (view == 1) result = diffuseLit * kd;
    else if (view == 2) result = specular;
    else if (view == 3) result = kd;
    else if (view == 4) result = vec3(ambientAo * cavity);
    else if (view == 5) result = vec3(firstShadow);
    else if (view == 6) result = N * 0.5 + 0.5;
    else if (view == 7) result = Ng * 0.5 + 0.5;
    else if (view == 8) result = firstThrough;
    else if (view == 9) result = blurred.a > 0.0 ? blurred.rgb : (diffuse + transmitted);
    else if (view == 10) result = diffuseColor;
    else if (view == 11) result = vec3(blurred.a);
    fragColor = vec4(result, 1.0);
#endif
}
