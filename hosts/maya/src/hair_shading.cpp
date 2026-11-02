// SPDX-License-Identifier: Apache-2.0
#include "hair_shading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include <maya/MDagPathArray.h>
#include <maya/MDrawContext.h>
#include <maya/MFloatArray.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MFnAttribute.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnSet.h>
#include <maya/MItDag.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MSelectionList.h>
#include <maya/MObjectArray.h>
#include <maya/MStateManager.h>
#include <maya/MTextureManager.h>
#include <maya/MViewport2Renderer.h>

#include <nlohmann/json.hpp>

#include "groomlab/groom_session.h"
#include "groomlab/hair_color.h"
#include "hair_bsdf_text.h"
#include "line_coverage_text.h"

namespace groomlab::maya {

namespace {

constexpr int kMaxLights = 8;
constexpr float kPi = 3.14159265358979f;

// ---- The effect's text ---------------------------------------------------------------------------
//
// The lobes are the app's (kHairBsdfText: engine/gpu/shaders/hair_bsdf.glsl in GLSL's words, its vector
// types renamed for HLSL by hlslOf), the shading around them written once here (kHairShared, in the
// words HLSL and GLSL share: mix is defined for HLSL, the vector types renamed for GLSL by glslOf),
// the coverage's hash the strand lines' (kLineCoverageText: engine/gpu/shaders/line_coverage.glsl,
// renamed for HLSL the same way), the rest per language: the uniforms, the vertex stage and the
// techniques.

// The material and the lights (uniforms the shared code reads), HLSL first.
const char* kHairUniformsHlsl = R"x(
float4 hairColor = float4(0.35, 0.25, 0.18, 1.0);  // the item's color (technique Hair)
float hairAzimuthalRoughness = 0.2;  // Arnold's: an RGB's absorption (hairColorSigma) by it
float4 hairWeights = float4(1.0, 1.0, 1.0, 0.21);  // the R, TT and TRT lobes' weights; w: the longitudinal width B
float4 hairLobes = float4(0.32, 1.55, 0.0524, 1.0); // azimuthal spread, ior, cuticle shift (radians), multiple scattering
float hairExposure = 3.14159265;  // Viewport 2.0's surfaces show color x light: pi x the render's radiance
float4 hairVariation = float4(0.0, 0.0, 0.0, 0.0);  // each strand's: specular, roughness (shares), cuticle tilt (radians), pigment (share)
float4 hairWetLook = float4(1.0, 0.25, 0.45, 0.0);  // wet hair, soaked (core's wetShadingUniform: hair_bsdf.glsl hairWet)
float hairDebug = 0.0;         // 1: the tangents as colors, 2: each strand's random numbers
float3 iridescenceColor = float3(0.05, 0.55, 0.2);
float iridescenceShift = 0.3;
float lineCoverage = 1.0;     // 0: every fragment as it is (ribbons)
float lineWidthScale = 1.0;
float lineCoverageFloor = 0.0;
float lineDensity = 1.0;
float lineLod = 0.0;
float lineBlend = 0.0;        // of two passes: 1 the opaque fragments alone, 2 the blended (0: both)
int gLightCount = 0;
float3 gAmbient = float3(0.0, 0.0, 0.0);
)x";

const char* kHairUniformsGlsl = R"x(
uniform vec4 hairColor = {0.35, 0.25, 0.18, 1.0};
uniform float hairAzimuthalRoughness = 0.2;
uniform vec4 hairWeights = {1.0, 1.0, 1.0, 0.21};
uniform vec4 hairLobes = {0.32, 1.55, 0.0524, 1.0};
uniform float hairExposure = 3.14159265;
uniform vec4 hairVariation = {0.0, 0.0, 0.0, 0.0};
uniform vec4 hairWetLook = {1.0, 0.25, 0.45, 0.0};
uniform float hairDebug = 0.0;
uniform vec3 iridescenceColor = {0.05, 0.55, 0.2};
uniform float iridescenceShift = 0.3;
uniform float lineCoverage = 1.0;
uniform float lineWidthScale = 1.0;
uniform float lineCoverageFloor = 0.0;
uniform float lineDensity = 1.0;
uniform float lineLod = 0.0;
uniform float lineBlend = 0.0;
uniform int gLightCount = 0;
uniform vec3 gAmbient = {0.0, 0.0, 0.0};
)x";

// Each light: gLightPos<i> (xyz the position; w the kind: 0 directional, 1 point, 2 spot, 3 area),
// gLightDir<i> (xyz the way the light travels: a directional's, a spot's axis, an area's normal),
// gLightColor<i> (rgb color x intensity; w the decay rate: 0 none, 1 linear, 2 quadratic, 3 cubic),
// gLightCone<i> (a spot's cosines: x the cone's edge, y inside its penumbra, z the dropoff).
std::string lightUniforms(bool glsl) {
    std::string text;
    for (int i = 0; i < kMaxLights; ++i) {
        for (const char* name : {"gLightPos", "gLightDir", "gLightColor", "gLightCone"}) {
            char line[96];
            std::snprintf(line, sizeof line, glsl ? "uniform vec4 %s%d = {0.0, 0.0, 0.0, 0.0};\n" : "float4 %s%d = float4(0.0, 0.0, 0.0, 0.0);\n",
                          name, i);
            text += line;
        }
    }
    return text;
}

// The shading around hair_bsdf.glsl's lobes (the standalone app's too, embedded: kHairBsdfText, in
// GLSL's words, hlslOf for DirectX 11): Viewport 2.0's lights, the strands' look colors and their
// variation. HLSL's words here, glslOf for GLSL.
const char* kHairShared = R"x(
// Viewport 2.0 takes hair_bsdf.glsl's calibration as it is (the app's GROOMLAB_APP_TUNE overrides it).
float4 hairCalibration() { return float4(0.0, 0.0, 0.0, 0.0); }
float4 hairCalibration2() { return float4(0.0, 0.0, 0.0, 0.0); }
float4 hairCalibration3() { return float4(0.0, 0.0, 0.0, 0.0); }

// A light of Viewport 2.0's at P (hairPreDraw's uniforms): its irradiance, and L toward it.
float3 hairIrradiance(float4 lp, float4 ld, float4 lc, float4 lx, float3 P, out float3 L) {
    if (lp.w < 0.5) {
        L = -normalize(ld.xyz);
        return lc.rgb;
    }
    float3 d = lp.xyz - P;
    float dist = max(length(d), 1e-4);
    L = d / dist;
    float atten = lc.w > 0.5 ? pow(dist, -lc.w) : 1.0;
    if (lp.w > 1.5 && lp.w < 2.5) {
        // A spot: within its cone (soft over the penumbra), its dropoff toward the edge.
        float c = dot(-L, normalize(ld.xyz));
        atten *= smoothstep(lx.x, max(lx.y, lx.x + 1e-4), c) * pow(max(c, 0.0), lx.z);
    } else if (lp.w > 2.5) {
        // An area light: from its center, by how squarely it faces P.
        atten *= max(dot(-L, normalize(ld.xyz)), 0.0);
    }
    return lc.rgb * atten;
}

// One light: hair_bsdf.glsl's lobes and dual scattering through the hairs the density grid puts in
// its way (hairCount: the hairs a ray crosses), the strand's own jitter on the march.
float3 hairLightAt(float4 lp, float4 ld, float4 lc, float4 lx, float3 P, float3 T, float3 V, float3 color, HairLobes m, float jitter,
                   float3 af, float3 ab) {
    float3 L;
    float3 E = hairIrradiance(lp, ld, lc, lx, P, L);
    if (dot(E, E) <= 0.0) return float3(0.0, 0.0, 0.0);
    return hairShadeLight(T, L, V, color, m, hairShadowCount(hairCount(P, L, jitter)), af, ab) * E;
}

// Viewport 2.0's light `i` (hairPreDraw's uniforms gLightPos0..7 and the rest, set by name).
void hairLightUniforms(int i, out float4 lp, out float4 ld, out float4 lc, out float4 lx) {
    lp = gLightPos0;
    ld = gLightDir0;
    lc = gLightColor0;
    lx = gLightCone0;
    if (i == 1) {
        lp = gLightPos1;
        ld = gLightDir1;
        lc = gLightColor1;
        lx = gLightCone1;
    }
    if (i == 2) {
        lp = gLightPos2;
        ld = gLightDir2;
        lc = gLightColor2;
        lx = gLightCone2;
    }
    if (i == 3) {
        lp = gLightPos3;
        ld = gLightDir3;
        lc = gLightColor3;
        lx = gLightCone3;
    }
    if (i == 4) {
        lp = gLightPos4;
        ld = gLightDir4;
        lc = gLightColor4;
        lx = gLightCone4;
    }
    if (i == 5) {
        lp = gLightPos5;
        ld = gLightDir5;
        lc = gLightColor5;
        lx = gLightCone5;
    }
    if (i == 6) {
        lp = gLightPos6;
        ld = gLightDir6;
        lc = gLightColor6;
        lx = gLightCone6;
    }
    if (i == 7) {
        lp = gLightPos7;
        ld = gLightDir7;
        lc = gLightColor7;
        lx = gLightCone7;
    }
}

// The fragment's color: the fiber's color from `strand` (rgb; a: 1 + its iridescence), wet by
// `wetness` (Wet's channel, hairWet), lit by every light. `rnd`, `rnd2`: the strand's own random
// numbers in [0, 1) (from its ID: the same at any preview density and level of detail), which vary
// its highlight's width, tilt and strength and its pigment as strands do (hairVariation).
float3 hairShade(float3 P, float3 T, float3 N, float4 strand, float wetness, float3 eye, float4 rnd, float4 rnd2) {
    float3 V = normalize(eye - P);
    if (hairDebug > 1.5 && hairDebug < 2.5) return rnd.xyz;
    if (hairDebug > 0.5 && hairDebug < 1.5) return 0.5 + 0.5 * T;
    // (3: the structural color's hue turned by each strand's random number: a check of hairHue's
    // fractions on every API.)
    if (hairDebug > 2.5 && hairDebug < 3.5) return hairHue(float3(0.9, 0.2, 0.1), rnd.x);
    float4 vary = hairVariation;
    HairLobes m;
    m.shape = float4(max(hairWeights.w * (1.0 + vary.y * (2.0 * rnd.x - 1.0)), 0.02), hairLobes.x,
                     hairLobes.z + vary.z * (2.0 * rnd.y - 1.0), hairLobes.y);
    float specular = max(hairWeights.x * (1.0 + vary.x * (2.0 * rnd.z - 1.0)), 0.0);
    m.weightTT = float3(hairWeights.y, hairWeights.y, hairWeights.y);
    m.weightTRT = float3(hairWeights.z, hairWeights.z, hairWeights.z);
    m.multiple = hairLobes.w;
    // The strand's own pigment: its absorption a share more or less.
    float pigment = 1.0 + vary.w * (2.0 * rnd2.y - 1.0);
    // Structural color (FeatherColor's iridescence): the fiber's color and its highlight's tint go
    // toward it, its hue turned as the view leaves the strand's normal.
    float3 tintR = float3(1.0, 1.0, 1.0);
    float3 tint = float3(1.0, 1.0, 1.0);
    float iridescent = clamp(strand.a - 1.0, 0.0, 1.0);
    if (iridescent > 0.0) {
        tint = hairHue(iridescenceColor, iridescenceShift * (1.0 - abs(dot(V, normalize(N)))));
        tintR = mix(tintR, tint, iridescent);
    }
    // The hair color (HairColor's RGB, the structural color mixed in: Arnold's base color, the
    // iridescence network's mix), absorbed as aiStandardHair absorbs its base color.
    float3 look = mix(strand.rgb, tint, iridescent);
    float3 color = hairFiberColor(hairColorSigma(look, hairAzimuthalRoughness) * pigment, float3(1.0, 1.0, 1.0));
    m.weightR = specular * tintR;
    hairWet(wetness, hairWetLook, color, m);
    float3 af;
    float3 ab;
    hairAttenuations(color, af, ab);
    float jitter = rnd2.x;
    float3 result = float3(0.0, 0.0, 0.0);
    // One light at a time, the loop bounded by the uniform count (not unrolled: the effect compiles
    // in a fraction of the time eight inlined copies of the shading took).
    for (int i = 0; i < gLightCount; ++i) {
        float4 lp, ld, lc, lx;
        hairLightUniforms(i, lp, ld, lc, lx);
        result += hairLightAt(lp, ld, lc, lx, P, T, V, color, m, jitter, af, ab);
    }
    // Ambient lights: light from everywhere, by the fiber's albedo (the app's environment
    // irradiance term), over pi as Viewport 2.0's own ambient shows it once times the exposure.
    result += gAmbient * hairAmbientAlbedo(color, m.multiple) * (hairOcclusion(P) / 3.14159265);
    return result * hairExposure;
}
)x";

// The density grid (HairGroomState): its texture and where it is, per language; then the march
// toward a light and the occlusion, shared.
const char* kGridHlsl = R"x(
Texture3D gDensityTexture;
SamplerState gDensitySampler {
    Filter = MIN_MAG_MIP_LINEAR;
    AddressU = Clamp;
    AddressV = Clamp;
    AddressW = Clamp;
};
float4 gGridOrigin = float4(0.0, 0.0, 0.0, 1.0);  // xyz the first cell's corner; w a cell's size
float4 gGridSize = float4(1.0, 1.0, 1.0, 0.0);    // the cells along x, y, z; w 1 with a grid
float4 gGridParams = float4(0.0, 1.3, 24.0, 1.0); // x how opaque (displaySelfShadowing), y each step's growth, z the most steps, w the occlusion's strength
float2 hairGridSample(float3 uvw) { return gDensityTexture.SampleLevel(gDensitySampler, uvw, 0).rg; }
)x";

const char* kGridGlsl = R"x(
uniform texture3D gDensityTexture;
uniform sampler3D gDensitySampler = sampler_state {
    Texture = <gDensityTexture>;
};
uniform vec4 gGridOrigin = {0.0, 0.0, 0.0, 1.0};
uniform vec4 gGridSize = {1.0, 1.0, 1.0, 0.0};
uniform vec4 gGridParams = {0.0, 1.3, 24.0, 1.0};
)x";

const char* kGridSampleGlsl = R"x(
    vec2 hairGridSample(vec3 uvw) { return textureLod(gDensitySampler, uvw, 0.0).rg; }
)x";

// The hairs in the way from P toward L (their optical depth): the grid's density sampled every
// step (trilinear, at the fragment's own place: neighboring strands differ) out of the grid,
// starting half a step plus `jitter` steps out (the strand's own fibers stay out of it; the
// jitter, the strand's own, keeps neighboring strands from taking the same steps). And the
// ambient occlusion at P, as opaque as the shadows.
const char* kGridShared = R"x(
float hairCount(float3 P, float3 L, float jitter) {
    if (gGridSize.w < 0.5 || gGridParams.x <= 0.0) return 0.0;
    // Steps of a cell near P (the strand's own neighborhood: its clump's shadow), each the next a
    // third longer out (the shadow from far off is smooth), at most gGridParams.z of them.
    float step = gGridOrigin.w;
    float3 extent = gGridSize.xyz * gGridOrigin.w;
    float t = (0.5 + jitter) * step;
    float tau = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (float(i) >= gGridParams.z) break;
        float3 uvw = (P + L * t - gGridOrigin.xyz) / extent;
        if (uvw.x < 0.0 || uvw.y < 0.0 || uvw.z < 0.0 || uvw.x > 1.0 || uvw.y > 1.0 || uvw.z > 1.0) break;
        tau += hairGridSample(uvw).x * step;
        if (tau * gGridParams.x > 6.0) break;  // (nothing more gets through)
        t += step;
        step *= gGridParams.y;
    }
    return tau * gGridParams.x;
}float hairOcclusion(float3 P) {
    if (gGridSize.w < 0.5 || gGridParams.x <= 0.0) return 1.0;
    float3 uvw = clamp((P - gGridOrigin.xyz) / (gGridSize.xyz * gGridOrigin.w), 0.0, 1.0);
    return pow(clamp(hairGridSample(uvw).y, 0.0001, 1.0), gGridParams.x * gGridParams.w);
}
)x";

// Every word "from" in text as "to" (whole identifiers only).
std::string replaceWords(std::string text, const std::string& from, const std::string& to) {
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
        const bool before = at > 0 && word(text[at - 1]);
        const bool after = at + from.size() < text.size() && word(text[at + from.size()]);
        if (before || after) {
            at = text.find(from, at + 1);
            continue;
        }
        text.replace(at, from.size(), to);
        at = text.find(from, at + to.size());
    }
    return text;
}

std::string glslOf(const std::string& shared) {
    std::string text = shared;
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}}) {
        text = replaceWords(text, from, to);
    }
    return text;
}

// hair_bsdf.glsl or line_coverage.glsl in the effect's language: GLSL's words as they are, HLSL's from them.
std::string hlslOf(const std::string& shared) {
    std::string text = shared;
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"vec2", "float2"}, {"vec3", "float3"}, {"vec4", "float4"},
                                   {"uvec2", "uint2"}, {"uvec3", "uint3"}, {"uvec4", "uint4"}}) {
        text = replaceWords(text, from, to);
    }
    return text;
}

// HLSL: what GLSL names otherwise.
const char* kHlslWords = R"x(
float mix(float a, float b, float t) { return lerp(a, b, t); }
float3 mix(float3 a, float3 b, float t) { return lerp(a, b, t); }
float3 mix(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
)x";

// The coverage's vertex arithmetic on the effect's uniforms (line_coverage.glsl lineCoverageOf;
// lineCoverage 0: off, every point opaque), HLSL.
const char* kCoverageHlsl = R"x(
float2 coverage(float width, float w) {
    if (lineCoverage < 0.5) return float2(1.0, 1.0);
    return lineCoverageOf(width, w, lineWidthScale, 0.5 * gProjXf[1][1] * gViewport.y, lineCoverageFloor, lineDensity, lineLod);
}
)x";

std::string hlslText(const std::string& grid) {
    std::string text = R"x(// GroomLab hair (DirectX 11): hair_shading.h
float4x4 gWVPXf : WorldViewProjection;
float4x4 gWXf : World;
float4x4 gProjXf : Projection;
float2 gViewport : ViewportPixelSize;
float3 gEye : WorldCameraPosition;
)x";
    text += kHairUniformsHlsl;
    text += lightUniforms(false);
    text += kHlslWords;
    text += hlslOf(kLineCoverageText);
    text += kCoverageHlsl;
    text += kGridHlsl;
    text += grid;
    text += hlslOf(kHairBsdfText);
    text += kHairShared;
    text += R"x(
struct appdata {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float2 Uv : TEXCOORD0;
};

struct appdataColored {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};

struct vertexOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;     // rgb the strand's color; a: 1 + its iridescence
    float3 Pw : TEXCOORD0;
    float3 Tw : TEXCOORD1;
    float3 Nw : TEXCOORD2;
    float4 Cover : TEXCOORD3;  // x: the coverage, y: the strand's own (keepShare), z: u plus twice the strand's number, w: the wetness
};

struct pixelOutput {
    float4 Color : SV_Target;
    uint Mask : SV_Coverage;
};

vertexOutput hairVertex(float3 position, float3 normal, float3 tangent, float2 uv, float4 color) {
    vertexOutput OUT;
    OUT.HPosition = mul(float4(position, 1.0), gWVPXf);
    OUT.Pw = mul(float4(position, 1.0), gWXf).xyz;
    OUT.Tw = mul(float4(tangent, 0.0), gWXf).xyz;
    OUT.Nw = mul(float4(normal, 0.0), gWXf).xyz;
    // The alpha's iridescence and wetness unpacked here, per vertex (packed, they would not interpolate).
    float2 look = hairLookAlpha(color.a, 1.0);
    OUT.Color = float4(color.rgb, 1.0 + look.x);
    float2 c = coverage(uv.y, OUT.HPosition.w);
    OUT.Cover = float4(c.x, c.y, uv.x, look.y);
    return OUT;
}

vertexOutput hairVS(appdata IN) { return hairVertex(IN.Position, IN.Normal, IN.Tangent, IN.Uv, hairColor); }
vertexOutput hairColoredVS(appdataColored IN) { return hairVertex(IN.Position, IN.Normal, IN.Tangent, IN.Uv, IN.Color); }

pixelOutput hairPS(vertexOutput IN) {
    float perPixel = length(float2(ddx(IN.Cover.z), ddy(IN.Cover.z)));
    float number = floor(0.5 * IN.Cover.z);
    uint strand = uint(max(number, 0.0));
    uint samples = GetRenderTargetSampleCount();
    float t = lineThreshold(strand, IN.Cover.z - 2.0 * number, perPixel, samples);
    float c = IN.Cover.x;
    float keep = samples > 1u ? c : lineKeepShare(c, IN.Cover.y);
    uint mask = lineSampleMask(keep, t, strand, samples);
    if (mask == 0u) discard;
    bool opaque = c >= 0.9999 * keep;
    if (lineBlend > 1.5 ? opaque : (lineBlend > 0.5 && !opaque)) discard;
    float3 T = normalize(IN.Tw);
    pixelOutput OUT;
    // (Straight from the hash, 24 bits each: fractions of sums of them came out 0 under Viewport 2.0's HLSL compiler.)
    float4 rnd = float4(float3(linePcg3d(uint3(strand, 0xA511u, 0x7Bu)) >> 8u) * (1.0 / 16777216.0), 0.0);
    float4 rnd2 = float4(float3(linePcg3d(uint3(strand, 0x51EDu, 0x3Cu)) >> 8u) * (1.0 / 16777216.0), 0.0);
    OUT.Color = float4(hairShade(IN.Pw, T, IN.Nw, IN.Color, IN.Cover.w, gEye, rnd, rnd2), c / max(keep, 1e-6));
    OUT.Mask = mask;
    return OUT;
}

technique11 Hair {
    pass p0 {
        SetVertexShader(CompileShader(vs_5_0, hairVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, hairPS()));
    }
}

technique11 HairColored {
    pass p0 {
        SetVertexShader(CompileShader(vs_5_0, hairColoredVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, hairPS()));
    }
}

// Game cards shaded as the strands are (technique HairCard): the baked color map (sRGB, its
// ambient occlusion in it) cut out by the coverage in its alpha, as the cards' own effect cuts it;
// the strand direction up the card (the tangents), the shading normal in the bitangents.
Texture2D map;
SamplerState mapSampler {
    Filter = MIN_MAG_LINEAR_MIP_POINT;
    AddressU = Clamp;
    AddressV = Clamp;
};
float cutoff = 0.5;
float solid = 1.0;            // 1: no maps (the solidColor)
float3 solidColor = float3(0.58, 0.50, 0.42);
float mapSize = 2048.0;
float cardPass = 0.0;         // 1: a shadow map's depth, 2: a point light's (cardPassPreDraw)
// The color map's Rec.709 to the rendering space, by rows (renderingSpace(): as a file node reads it).
float3 cardFromRec709R = float3(1.0, 0.0, 0.0);
float3 cardFromRec709G = float3(0.0, 1.0, 0.0);
float3 cardFromRec709B = float3(0.0, 0.0, 1.0);
float gLightRange : LightRange;

// The samples of the pixel's `samples` a card of map coverage `a` at `uv` covers, none clipped.
// Magnified: cut at the cutoff, across a ramp a pixel wide on a multisampled target (the viewport's
// anti-aliasing), so the samples smooth the edge (alpha to coverage's, decorrelated: a run of
// samples turned by a hash of the pixel's place on the card, line_coverage.glsl lineSampleMask, so
// cards over each other cover samples of their own), else hard. A texel or more to a pixel: the
// coverage kept as the texture shrinks (each mip level down averages thin strands with the gaps
// beside them, so a quarter more a level), that share of the samples, or on one sample a pixel
// kept at a threshold hashed per pixel (in uv), so a strand thinner than a pixel shows as often as
// it covers it instead of not at all. The derivatives are taken before any branch.
uint cardMask(float a, float2 uv, uint samples) {
    float2 footprint = max(fwidth(uv), float2(1e-7, 1e-7));
    float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6)), 0.0);
    float kept = a * (1.0 + 0.25 * lod);
    float ramp = max(fwidth(kept), 1e-4);
    float2 cell = floor(uv / footprint);
    float noise = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
    if (samples > 1u) {
        float c = lerp(saturate((kept - cutoff) / ramp + 0.5), saturate(kept), saturate(lod));
        uint mask = lineSampleMask(c, max(noise, 1e-6), uint(noise * 16777216.0), samples);
        if (mask == 0u) discard;
        return mask;
    }
    clip(kept - lerp(cutoff, noise, saturate(lod)));
    return 0xFFFFFFFFu;
}

struct appdataCard {
    float3 Position : POSITION;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};

struct cardOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;     // rgb the card's tint (0.5: none); a: 1 + its iridescence
    float3 Pw : TEXCOORD0;
    float3 Tw : TEXCOORD1;
    float3 Nw : TEXCOORD2;
    float2 Uv : TEXCOORD3;
};

cardOutput hairCardVS(appdataCard IN) {
    cardOutput OUT;
    OUT.HPosition = mul(float4(IN.Position, 1.0), gWVPXf);
    OUT.Pw = mul(float4(IN.Position, 1.0), gWXf).xyz;
    OUT.Tw = mul(float4(IN.Tangent, 0.0), gWXf).xyz;
    OUT.Nw = mul(float4(IN.Bitangent, 0.0), gWXf).xyz;
    OUT.Color = IN.Color;
    OUT.Uv = IN.Uv;
    return OUT;
}

struct cardPixel {
    float4 Color : SV_Target;
    uint Mask : SV_Coverage;
};

cardPixel hairCardPS(cardOutput IN) {
    cardPixel OUT;
    OUT.Mask = 0xFFFFFFFFu;
    float3 base = solidColor;
    if (cardPass > 0.5) {
        // Viewport 2.0 draws the cards' shadows with this shader too: the shadow map's depth (Maya's
        // mayaShadower's), a point light's distance over its range, the card cut out as drawn.
        if (solid < 0.5) cardMask(map.Sample(mapSampler, IN.Uv).a, IN.Uv, 1u);
        float z = IN.HPosition.z;
        float d = length(IN.Pw - gEye) / max(gLightRange, 1e-6);
        OUT.Color = cardPass > 1.5 ? float4(d, 0.0, 0.0, d) : (z + fwidth(z)).xxxx;
        return OUT;
    }
    if (solid < 0.5) {
        float4 m = map.Sample(mapSampler, IN.Uv);
        OUT.Mask = cardMask(m.a, IN.Uv, GetRenderTargetSampleCount());
        float3 c = m.rgb;
        // The bake holds the authored base color in Rec.709; restore the rendering space.
        float3 rec709 = lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
        float3 baked = float3(dot(cardFromRec709R, rec709), dot(cardFromRec709G, rec709), dot(cardFromRec709B, rec709)) * 2.0 * IN.Color.rgb;
        base = clamp(baked, 1e-8, 1.0);
    }
    // (No strand of its own: the strands' variation is the bake's.)
    float4 none = float4(0.5, 0.5, 0.5, 0.5);
    OUT.Color = float4(hairShade(IN.Pw, normalize(IN.Tw), IN.Nw, float4(base, IN.Color.a), 0.0, gEye, none, none), 1.0);
    return OUT;
}

technique11 HairCard {
    pass p0 {
        SetVertexShader(CompileShader(vs_5_0, hairCardVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, hairCardPS()));
    }
}
)x";
    return text;
}

const char* kVertexGlsl = R"x(
    void hairVertex(vec3 position, vec3 normal, vec3 tangent, vec2 uv, vec4 color) {
        gl_Position = gWVPXf * vec4(position, 1.0);
        vsOut.Pw = (gWXf * vec4(position, 1.0)).xyz;
        vsOut.Tw = (gWXf * vec4(tangent, 0.0)).xyz;
        vsOut.Nw = (gWXf * vec4(normal, 0.0)).xyz;
        vec2 look = hairLookAlpha(color.a, 1.0);  // (the HLSL's notes)
        vsOut.Color = vec4(color.rgb, 1.0 + look.x);
        vec2 c = lineCoverage > 0.5 ? lineCoverageOf(uv.y, gl_Position.w, lineWidthScale, 0.5 * gProjXf[1][1] * gViewport.y,
                                                     lineCoverageFloor, lineDensity, lineLod)
                                    : vec2(1.0, 1.0);
        vsOut.Cover = vec4(c.x, c.y, uv.x, look.y);
    }
)x";

std::string glslText(const std::string& grid) {
    std::string text = R"x(#version 430
// GroomLab hair (OpenGL Core Profile: OGSFX): hair_shading.h
uniform mat4 gWVPXf : WorldViewProjection;
uniform mat4 gWXf : World;
uniform mat4 gProjXf : Projection;
uniform vec2 gViewport : ViewportPixelSize;
uniform vec3 gEye : WorldCameraPosition;
)x";
    text += kHairUniformsGlsl;
    text += lightUniforms(true);
    text += kGridGlsl;
    text += R"x(
attribute appdata {
    vec3 inPosition : POSITION;
    vec3 inNormal : NORMAL;
    vec3 inTangent : TANGENT;
    vec2 inUv : TEXCOORD0;
};

attribute appdataColored {
    vec3 inPosition : POSITION;
    vec3 inNormal : NORMAL;
    vec3 inTangent : TANGENT;
    vec2 inUv : TEXCOORD0;
    vec4 inColor : COLOR0;
};

attribute vertexOutput {
    vec4 Color;
    vec3 Pw;
    vec3 Tw;
    vec3 Nw;
    vec4 Cover;
};

attribute pixelOutput {
    vec4 outColor : COLOR0;
};

// Game cards shaded as the strands are (technique HairCard; the HLSL's notes).
uniform texture2D map;
uniform sampler2D mapSampler = sampler_state {
    Texture = <map>;
};
uniform float cutoff = 0.5;
uniform float solid = 1.0;
uniform vec3 solidColor = {0.58, 0.50, 0.42};
uniform float mapSize = 2048.0;
uniform float cardPass = 0.0;
uniform vec3 cardFromRec709R = {1.0, 0.0, 0.0};
uniform vec3 cardFromRec709G = {0.0, 1.0, 0.0};
uniform vec3 cardFromRec709B = {0.0, 0.0, 1.0};
uniform float gLightRange : LightRange;

attribute appdataCard {
    vec3 inPosition : POSITION;
    vec3 inTangent : TANGENT;
    vec3 inBitangent : BINORMAL;
    vec2 inUv : TEXCOORD0;
    vec4 inColor : COLOR0;
};

attribute cardOutput {
    vec4 Color;
    vec3 Pw;
    vec3 Tw;
    vec3 Nw;
    vec2 Uv;
};

GLSLShader hairVS {
)x";
    text += kLineCoverageText;
    text += kHairBsdfText;  // (hairLookAlpha)
    text += kVertexGlsl;
    text += R"x(
    void main() { hairVertex(inPosition, inNormal, inTangent, inUv, hairColor); }
}

GLSLShader hairColoredVS {
)x";
    text += kLineCoverageText;
    text += kHairBsdfText;
    text += kVertexGlsl;
    text += R"x(
    void main() { hairVertex(inPosition, inNormal, inTangent, inUv, inColor); }
}

GLSLShader hairPS {
)x";
    text += kLineCoverageText;
    text += kGridSampleGlsl;
    text += glslOf(grid);
    text += kHairBsdfText;
    text += glslOf(kHairShared);
    text += R"x(
    void main() {
        float perPixel = length(vec2(dFdx(psIn.Cover.z), dFdy(psIn.Cover.z)));
        float number = floor(0.5 * psIn.Cover.z);
        uint strand = uint(max(number, 0.0));
        uint samples = uint(max(gl_NumSamples, 1));
        float t = lineThreshold(strand, psIn.Cover.z - 2.0 * number, perPixel, samples);
        float c = psIn.Cover.x;
        float keep = samples > 1u ? c : lineKeepShare(c, psIn.Cover.y);
        uint mask = lineSampleMask(keep, t, strand, samples);
        if (mask == 0u) discard;
        bool opaque = c >= 0.9999 * keep;
        if (lineBlend > 1.5 ? opaque : (lineBlend > 0.5 && !opaque)) discard;
        gl_SampleMask[0] = int(mask);
        vec3 T = normalize(psIn.Tw);
        vec4 rnd = vec4(vec3(linePcg3d(uvec3(strand, 0xA511u, 0x7Bu)) >> 8u) * (1.0 / 16777216.0), 0.0);
        vec4 rnd2 = vec4(vec3(linePcg3d(uvec3(strand, 0x51EDu, 0x3Cu)) >> 8u) * (1.0 / 16777216.0), 0.0);
        outColor = vec4(hairShade(psIn.Pw, T, psIn.Nw, psIn.Color, psIn.Cover.w, gEye, rnd, rnd2), c / max(keep, 1e-6));
    }
}

technique Hair {
    pass p0 {
        VertexShader (in appdata, out vertexOutput vsOut) = hairVS;
        PixelShader (in vertexOutput psIn, out pixelOutput) = hairPS;
    }
}

technique HairColored {
    pass p0 {
        VertexShader (in appdataColored, out vertexOutput vsOut) = hairColoredVS;
        PixelShader (in vertexOutput psIn, out pixelOutput) = hairPS;
    }
}

GLSLShader hairCardVS {
    void main() {
        gl_Position = gWVPXf * vec4(inPosition, 1.0);
        cardOut.Pw = (gWXf * vec4(inPosition, 1.0)).xyz;
        cardOut.Tw = (gWXf * vec4(inTangent, 0.0)).xyz;
        cardOut.Nw = (gWXf * vec4(inBitangent, 0.0)).xyz;
        cardOut.Color = inColor;
        cardOut.Uv = inUv;
    }
}

GLSLShader hairCardPS {
)x";
    text += kLineCoverageText;
    text += kGridSampleGlsl;
    text += glslOf(grid);
    text += kHairBsdfText;
    text += glslOf(kHairShared);
    text += R"x(
    // (The HLSL's cardMask.)
    uint cardMask(float a, vec2 uv, uint samples) {
        vec2 footprint = max(fwidth(uv), vec2(1e-7, 1e-7));
        float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6)), 0.0);
        float kept = a * (1.0 + 0.25 * lod);
        float ramp = max(fwidth(kept), 1e-4);
        vec2 cell = floor(uv / footprint);
        float noise = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
        if (samples > 1u) {
            float c = mix(clamp((kept - cutoff) / ramp + 0.5, 0.0, 1.0), clamp(kept, 0.0, 1.0), clamp(lod, 0.0, 1.0));
            uint mask = lineSampleMask(c, max(noise, 1e-6), uint(noise * 16777216.0), samples);
            if (mask == 0u) discard;
            return mask;
        }
        if (kept - mix(cutoff, noise, clamp(lod, 0.0, 1.0)) < 0.0) discard;
        return 0xFFFFFFFFu;
    }

    void main() {
        vec3 base = solidColor;
        if (cardPass > 0.5) {
            // (The HLSL's notes; OpenGL's depth in [0, 1] as Maya's mayaShadower writes it.)
            if (solid < 0.5) cardMask(texture(mapSampler, cardIn.Uv).a, cardIn.Uv, 1u);
            float z = gl_FragCoord.z;
            float d = length(cardIn.Pw - gEye) / max(gLightRange, 1e-6);
            gl_SampleMask[0] = -1;
            outColor = cardPass > 1.5 ? vec4(d, 0.0, 0.0, d) : vec4(z + fwidth(z));
            return;
        }
        uint mask = 0xFFFFFFFFu;
        if (solid < 0.5) {
            vec4 m = texture(mapSampler, cardIn.Uv);
            mask = cardMask(m.a, cardIn.Uv, uint(max(gl_NumSamples, 1)));
            vec3 c = m.rgb;
            vec3 rec709 = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
            vec3 baked = vec3(dot(cardFromRec709R, rec709), dot(cardFromRec709G, rec709), dot(cardFromRec709B, rec709)) * 2.0 * cardIn.Color.rgb;
            base = clamp(baked, 1e-8, 1.0);
        }
        gl_SampleMask[0] = int(mask);
        vec4 none = vec4(0.5, 0.5, 0.5, 0.5);
        outColor = vec4(hairShade(cardIn.Pw, normalize(cardIn.Tw), cardIn.Nw, vec4(base, cardIn.Color.a), 0.0, gEye, none, none), 1.0);
    }
}

technique HairCard {
    pass p0 {
        VertexShader (in appdataCard, out cardOutput cardOut) = hairCardVS;
        PixelShader (in cardOutput cardIn, out pixelOutput) = hairCardPS;
    }
}
)x";
    return text;
}

// The effect in the draw API's language (empty: none).
// The effect's text with only technique `technique` of it (Hair, HairColored, HairCard): an effect
// compiles every technique it holds, and each is the whole hair shading, so one alone compiles in a
// third of the time (Viewport 2.0 compiles it on the first draw that asks for it, then keeps it).
std::string onlyTechnique(const std::string& text, const std::string& technique) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t found = text.find("\ntechnique", at);
        if (found == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        out.append(text, at, found + 1 - at);
        // The block: "technique[11] Name {" to its closing brace (the pass's own closes first).
        const std::size_t open = text.find('{', found);
        std::size_t depth = 0, end = open;
        for (; end < text.size(); ++end) {
            if (text[end] == '{') ++depth;
            if (text[end] == '}' && --depth == 0) break;
        }
        const std::size_t nameStart = text.find(' ', found + 1) + 1;
        const std::string name = text.substr(nameStart, text.find_first_of(" {", nameStart) - nameStart);
        if (name == technique) out.append(text, found + 1, end + 1 - (found + 1));
        at = end + 1;
    }
    return out;
}

const std::string& hairEffectText(const std::string& technique) {
    static std::map<std::string, std::string> hlsl, glsl;
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    switch (renderer ? renderer->drawAPI() : MHWRender::kNone) {
        case MHWRender::kDirectX11: {
            std::string& text = hlsl[technique];
            if (text.empty()) text = onlyTechnique(hlslText(kGridShared), technique);
            return text;
        }
        case MHWRender::kOpenGLCoreProfile: {
            std::string& text = glsl[technique];
            if (text.empty()) text = onlyTechnique(glslText(kGridShared), technique);
            return text;
        }
        default: {
            static const std::string none;
            return none;
        }
    }
}

// ---- The lights ------------------------------------------------------------------------------------

struct HairLight {
    float pos[4] = {}, dir[4] = {}, color[4] = {}, cone[4] = {};
    MDagPath path;  // (invalid: the default light)
    std::string type;
};

struct HairLightSet {
    int count = 0;
    HairLight lights[kMaxLights];
    float ambient[3] = {};
    std::uint64_t hash = 0;
};

// The draw context's lights, read once a frame and pass (every hair draw of it takes the same).
struct LightCache {
    MUint64 frame = ~MUint64{0};
    MString pass;
    std::vector<HairLight> lights;
    float ambient[3] = {};
    std::vector<MDagPath> ambientPaths;
    std::vector<std::array<float, 3>> ambientColors;
};

float firstFloat(MHWRender::MLightParameterInformation& info, MHWRender::MLightParameterInformation::StockParameterSemantic s, float fallback,
                 MFloatArray* all = nullptr) {
    MFloatArray values;
    if (info.getParameter(s, values) == MS::kSuccess && values.length() > 0) {
        if (all) *all = values;
        return values[0];
    }
    MIntArray ints;
    if (info.getParameter(s, ints) == MS::kSuccess && ints.length() > 0) return static_cast<float>(ints[0]);
    return fallback;
}

void readLights(const MHWRender::MDrawContext& context, LightCache& cache) {
    using P = MHWRender::MLightParameterInformation;
    cache.lights.clear();
    cache.ambientPaths.clear();
    cache.ambientColors.clear();
    cache.ambient[0] = cache.ambient[1] = cache.ambient[2] = 0.0f;
    const unsigned n = context.numberOfActiveLights(MHWRender::MDrawContext::kFilteredToLightLimit);
    for (unsigned i = 0; i < n; ++i) {
        MHWRender::MLightParameterInformation* info = context.getLightParameterInformation(i, MHWRender::MDrawContext::kFilteredToLightLimit);
        if (!info) continue;
        if (firstFloat(*info, P::kLightEnabled, 1.0f) < 0.5f) continue;
        const float intensity = firstFloat(*info, P::kIntensity, 1.0f);
        MFloatArray color;
        firstFloat(*info, P::kColor, 1.0f, &color);
        float rgb[3] = {1.0f, 1.0f, 1.0f};
        for (unsigned k = 0; k < 3 && k < color.length(); ++k) rgb[k] = color[k];
        HairLight l;
        l.type = info->lightType().asChar();
        MStatus status;
        l.path = info->lightPath(&status);
        if (!status) l.path = MDagPath();
        std::string lower = l.type;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("ambient") != std::string::npos) {
            std::array<float, 3> a{rgb[0] * intensity, rgb[1] * intensity, rgb[2] * intensity};
            cache.ambientPaths.push_back(l.path);
            cache.ambientColors.push_back(a);
            continue;
        }
        MFloatArray position, direction;
        info->getParameter(P::kWorldPosition, position);
        info->getParameter(P::kWorldDirection, direction);
        float kind = 1.0f;  // point
        if (lower.find("directional") != std::string::npos || (position.length() < 3 && direction.length() >= 3)) {
            kind = 0.0f;
        } else if (lower.find("spot") != std::string::npos) {
            kind = 2.0f;
        } else if (lower.find("area") != std::string::npos) {
            kind = 3.0f;
        }
        if (kind > 0.5f && position.length() < 3) continue;  // (nothing to light from)
        if (kind < 0.5f && direction.length() < 3) continue;
        // The position: an area light's corners' center (its four corners), else the one.
        float p[3] = {0.0f, 0.0f, 0.0f};
        const unsigned corners = position.length() / 3;
        for (unsigned c = 0; c < corners; ++c) {
            for (int k = 0; k < 3; ++k) p[k] += position[c * 3 + k] / static_cast<float>(corners);
        }
        for (int k = 0; k < 3; ++k) {
            l.pos[k] = p[k];
            l.dir[k] = direction.length() >= 3 ? direction[k] : 0.0f;
            l.color[k] = rgb[k] * intensity;
        }
        l.pos[3] = kind;
        l.color[3] = kind > 0.5f ? std::clamp(firstFloat(*info, P::kDecayRate, 0.0f), 0.0f, 3.0f) : 0.0f;
        if (kind > 1.5f && kind < 2.5f) {
            MFloatArray cone;
            firstFloat(*info, P::kCosConeAngle, 0.5f, &cone);
            float edge = cone.length() > 0 ? cone[0] : 0.5f, inside = edge + 0.02f;
            if (cone.length() > 1) {
                edge = std::min(cone[0], cone[1]);
                inside = std::max(cone[0], cone[1]);
            }
            l.cone[0] = edge;
            l.cone[1] = std::max(inside, edge + 1e-4f);
            l.cone[2] = std::max(firstFloat(*info, P::kDropoff, 0.0f), 0.0f);
        }
        cache.lights.push_back(l);
    }
}

std::uint64_t mixHash(std::uint64_t h, const void* data, std::size_t bytes) {
    const auto* b = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

bool ignored(const MDagPath& path, const std::vector<MDagPath>& ignore) {
    if (!path.isValid() || ignore.empty()) return false;
    for (const MDagPath& p : ignore) {
        if (p == path) return true;
    }
    return false;
}

HairLightSet lightsFor(const LightCache& cache, const std::vector<MDagPath>& ignore) {
    HairLightSet set;
    for (const HairLight& l : cache.lights) {
        if (set.count >= kMaxLights) break;
        if (ignored(l.path, ignore)) continue;
        set.lights[set.count++] = l;
    }
    for (std::size_t i = 0; i < cache.ambientColors.size(); ++i) {
        if (ignored(cache.ambientPaths[i], ignore)) continue;
        for (int k = 0; k < 3; ++k) set.ambient[k] += cache.ambientColors[i][k];
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = mixHash(h, &set.count, sizeof set.count);
    for (int i = 0; i < set.count; ++i) {
        const HairLight& l = set.lights[i];
        h = mixHash(h, l.pos, sizeof l.pos);
        h = mixHash(h, l.dir, sizeof l.dir);
        h = mixHash(h, l.color, sizeof l.color);
        h = mixHash(h, l.cone, sizeof l.cone);
    }
    h = mixHash(h, set.ambient, sizeof set.ambient);
    set.hash = h | 1u;
    return set;
}

// Each hair shader's draw state: its groom's, whether it draws the strand lines' coverage (the
// blending callbacks), and the lights it was last given.
struct HairShaderEntry {
    // (Not held: the groom's draw override owns it, and its grid texture goes with the override,
    // never at the plugin's unloading, when the renderer is gone.)
    std::weak_ptr<HairGroomState> groom;
    bool coverage = false;
    bool card = false;  // technique HairCard: the pass handed to it (cardPassPreDraw)
    std::uint64_t lightsHash = 0;
    // The density grid it was last given (setHairGrid): the texture's number, the grid's.
    std::uint64_t gridVolume = 0, gridSerial = 0;
};

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<const MHWRender::MShaderInstance*, HairShaderEntry>& registry() {
    static std::unordered_map<const MHWRender::MShaderInstance*, HairShaderEntry> r;
    return r;
}

// The last ignoredLights' counts (hairLightsJson): the groom's nodes and sets, the ignore and link
// entries naming them, the lights lighting everything, the lights ignored.
std::array<std::size_t, 5>& lastLinking() {
    static std::array<std::size_t, 5> counts{};
    return counts;
}

LightCache& lightCache() {
    static LightCache c;
    return c;
}

// The last hair draw's lights (hairLightsJson) and how many hair draws there were.
struct HairDrawStats {
    HairLightSet last;
    long long draws = 0, lightUpdates = 0;
    std::size_t ignored = 0;  // the lights the last hair draw's groom ignores
};
HairDrawStats& drawStats() {
    static HairDrawStats s;
    return s;
}

void setLights(MHWRender::MShaderInstance& shader, const HairLightSet& set) {
    static const char* const kNames[4] = {"gLightPos", "gLightDir", "gLightColor", "gLightCone"};
    for (int i = 0; i < kMaxLights; ++i) {
        const HairLight empty;
        const HairLight& l = i < set.count ? set.lights[i] : empty;
        const float* values[4] = {l.pos, l.dir, l.color, l.cone};
        for (int k = 0; k < 4; ++k) {
            MString name(kNames[k]);
            name += i;
            shader.setParameter(name, values[k]);
        }
    }
    shader.setParameter("gLightCount", set.count);
    shader.setParameter("gAmbient", set.ambient);
}

std::atomic<long long>& unknownDraws() {
    static std::atomic<long long> n{0};
    return n;
}

void hairPreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance* shader) {
    if (!shader) return;
    HairShaderEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto it = registry().find(shader);
        if (it != registry().end()) entry = &it->second;
    }
    if (!entry) ++unknownDraws();
    if (entry && entry->coverage) coveragePreDraw(context, items, shader);
    if (entry && entry->card) cardPassPreDraw(context, shader);
    if (!colorPass(context)) return;
    LightCache& cache = lightCache();
    const MUint64 frame = context.getFrameStamp();
    const MString pass = context.getPassContext().passIdentifier();
    if (frame != cache.frame || pass != cache.pass) {
        readLights(context, cache);
        cache.frame = frame;
        cache.pass = pass;
    }
    static const std::vector<MDagPath> kNone;
    const std::shared_ptr<HairGroomState> groom = entry ? entry->groom.lock() : nullptr;
    const std::vector<MDagPath>& ignore = groom ? groom->ignoredLights : kNone;
    const HairLightSet set = lightsFor(cache, ignore);
    HairDrawStats& stats = drawStats();
    stats.ignored = ignore.size();
    ++stats.draws;
    stats.last = set;
    if (entry && entry->lightsHash == set.hash) return;
    setLights(*shader, set);
    shader->updateParameters(context);
    ++stats.lightUpdates;
    if (entry) entry->lightsHash = set.hash;
}

void hairPostDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance* shader) {
    bool coverage = false;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto it = registry().find(shader);
        coverage = it != registry().end() && it->second.coverage;
    }
    if (coverage) coveragePostDraw(context, items, shader);
}

}  // namespace

MHWRender::MShaderInstance* acquireHairShader(const MHWRender::MShaderManager& shaders, bool colored, bool coverage,
                                              const std::shared_ptr<HairGroomState>& groom, bool card) {
    static int state = 0;  // 0 not tried, 1 compiles, -1 failed
    if (state < 0) return nullptr;
    const char* technique = card ? "HairCard" : colored ? "HairColored" : "Hair";
    const std::string& text = hairEffectText(technique);
    if (text.empty()) return nullptr;
    MHWRender::MShaderInstance* shader = shaders.getEffectsBufferShader(
        text.c_str(), static_cast<unsigned>(text.size()), technique, nullptr, 0, true,
        hairPreDraw, hairPostDraw);
    if (!shader) {
        state = -1;
        MGlobal::displayWarning(MString("GroomLab: the hair shader did not compile, so the lit strands draw with Maya's hair shading: ") +
                                MHWRender::MShaderManager::getLastError());
        return nullptr;
    }
    state = 1;
    // Hashed in the opaque pass, depth written (the blended top of the coverage: coveragePreDraw).
    shader->setIsTransparent(false);
    if (card) noteCardShader(shader);
    std::lock_guard<std::mutex> lock(registryMutex());
    HairShaderEntry& entry = registry()[shader];  // (a released instance's address may come again: made anew)
    entry = HairShaderEntry{};
    entry.groom = groom;
    entry.coverage = coverage;
    entry.card = card;
    return shader;
}

bool hairShaderGrid(const MHWRender::MShaderInstance* shader, std::uint64_t* volume, std::uint64_t* serial) {
    std::lock_guard<std::mutex> lock(registryMutex());
    const auto it = registry().find(shader);
    if (it == registry().end()) return false;
    if (volume) *volume = it->second.gridVolume;
    if (serial) *serial = it->second.gridSerial;
    return true;
}

int noteHairShader(const MHWRender::MShaderInstance* shader, bool coverage, const std::shared_ptr<HairGroomState>& groom) {
    if (!shader) return -1;
    std::lock_guard<std::mutex> lock(registryMutex());
    auto [it, added] = registry().try_emplace(shader);
    const int was = added ? -1 : (it->second.coverage ? 1 : 0);
    HairShaderEntry& entry = it->second;
    if (entry.coverage != coverage || entry.groom.lock() != groom) entry.lightsHash = 0;  // (the lights given again)
    entry.coverage = coverage;
    entry.groom = groom;
    return was;
}

int hairShaderCoverage(const MHWRender::MShaderInstance* shader) {
    std::lock_guard<std::mutex> lock(registryMutex());
    const auto it = registry().find(shader);
    return it == registry().end() ? -1 : (it->second.coverage ? 1 : 0);
}

long long hairDrawsUnknown() { return unknownDraws(); }

bool hairShaderAvailable(const MHWRender::MShaderManager& shaders) {
    static int state = 0;
    static MHWRender::DrawAPI api = MHWRender::kNone;
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    const MHWRender::DrawAPI now = renderer ? renderer->drawAPI() : MHWRender::kNone;
    if (state == 0 || api != now) {
        api = now;
        MHWRender::MShaderInstance* probe = acquireHairShader(shaders, true, false, nullptr);  // (HairColored: what the lit strands draw with, so their first draw finds it compiled)
        state = probe ? 1 : -1;
        if (probe) shaders.releaseShader(probe);
    }
    return state > 0;
}

HairGroomState::~HairGroomState() {
    if (!volume) return;
    if (MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer()) {
        if (MHWRender::MTextureManager* textures = renderer->getTextureManager()) textures->releaseTexture(volume);
    }
}

MHWRender::MTexture* HairGroomState::volumeFor(const unsigned want[3]) {
    if (volume && !remake && size[0] == want[0] && size[1] == want[1] && size[2] == want[2]) return volume;
    remake = false;
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    if (!textures) return nullptr;
    // The new texture made before the old one goes (another address, never the freed one's: a
    // shader handed a texture at the address it already holds may keep drawing the old one), under
    // a name of its own (the texture manager hands back a cached texture of the same name).
    MHWRender::MTexture* old = volume;
    volume = nullptr;
    valid = false;
    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = want[0];
    desc.fHeight = want[1];
    desc.fDepth = want[2];
    desc.fTextureType = MHWRender::kVolumeTexture;
    desc.fFormat = MHWRender::kR32G32_FLOAT;
    desc.fMipmaps = 1;
    desc.fArraySlices = 1;
    desc.fBytesPerRow = want[0] * 8;
    desc.fBytesPerSlice = want[0] * want[1] * 8;
    static std::atomic<unsigned long long> made{0};
    char name[128];
    std::snprintf(name, sizeof name, "groomLabHairDensity_%p_%ux%ux%u_%llu", static_cast<void*>(this), want[0], want[1], want[2],
                  ++made);
    const std::vector<float> zeros(std::size_t{want[0]} * want[1] * want[2] * 2, 0.0f);
    volume = textures->acquireTexture(MString(name), desc, zeros.data(), false);
    if (old) textures->releaseTexture(old);
    if (volume) ++volumeSerial;
    for (int k = 0; k < 3; ++k) size[k] = volume ? want[k] : 0;
    return volume;
}

void setHairGrid(MHWRender::MShaderInstance& shader, const HairGroomState* groom, float selfShadowing) {
    const bool on = groom && groom->valid && groom->volume && selfShadowing > 0.0f;
    // The texture this shader was given last: another one (made anew after the grid changed size,
    // a groom scaled with its scalp say) is bound after none, so the instance lets go of the old
    // one whatever address the new one has.
    bool rebind = on;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        const auto it = registry().find(&shader);
        if (it != registry().end()) {
            const std::uint64_t volume = on ? groom->volumeSerial : 0;
            rebind = on && it->second.gridVolume != volume;
            it->second.gridVolume = volume;
            it->second.gridSerial = on ? groom->serial : 0;
        }
    }
    if (on) {
        MHWRender::MTextureAssignment assignment;
        if (rebind) {
            assignment.texture = nullptr;
            shader.setParameter("gDensityTexture", assignment);
        }
        assignment.texture = groom->volume;
        shader.setParameter("gDensityTexture", assignment);
        static const MHWRender::MSamplerState* sampler = [] {
            MHWRender::MSamplerStateDesc desc;
            desc.filter = MHWRender::MSamplerState::kMinMagMipLinear;
            desc.addressU = desc.addressV = desc.addressW = MHWRender::MSamplerState::kTexClamp;
            return MHWRender::MStateManager::acquireSamplerState(desc);
        }();
        if (sampler) shader.setParameter("gDensitySampler", *sampler);
        const float origin[4] = {groom->origin[0], groom->origin[1], groom->origin[2], groom->cell};
        shader.setParameter("gGridOrigin", origin);
    }
    const float size[4] = {on ? static_cast<float>(groom->size[0]) : 1.0f, on ? static_cast<float>(groom->size[1]) : 1.0f,
                           on ? static_cast<float>(groom->size[2]) : 1.0f, on ? 1.0f : 0.0f};
    shader.setParameter("gGridSize", size);
    // Steps of a cell growing by a third each, 24 at most (they cross 1800 cells), each fragment's
    // march ended where nothing more gets through.
    const float params[4] = {on ? selfShadowing : 0.0f, 1.3f, 24.0f, 1.0f};
    shader.setParameter("gGridParams", params);
}

void setHairMaterial(MHWRender::MShaderInstance& shader, const float color[4], const HairLook& look, float variation, int debug) {
    shader.setParameter("hairColor", color);
    // HairColor's highlight as create_hair_shader gives aiStandardHair (core's hairLobes: the R
    // lobe's tint, the roughness, the cuticle tilt; the rest aiStandardHair's defaults).
    const HairLobes arnold = hairLobes(look);
    const float weights[4] = {arnold.specularTint, 1.0f, 1.0f, hairLongitudinalWidth(arnold.roughness)};
    const float lobes[4] = {hairAzimuthalSpread(arnold.azimuthalRoughness), arnold.ior, arnold.shiftDegrees * kPi / 180.0f, 1.0f};
    shader.setParameter("hairWeights", weights);
    shader.setParameter("hairLobes", lobes);
    shader.setParameter("hairAzimuthalRoughness", arnold.azimuthalRoughness);
    shader.setParameter("hairExposure", kPi);
    shader.setParameter("iridescenceColor", look.iridescenceColor.data());
    shader.setParameter("iridescenceShift", look.iridescenceShift);
    // Each strand's variation: HairColor's specular and roughness randomness (as it renders), and
    // the viewport's own on top (`variation`): the cuticle tilt +/- 4 degrees, the highlight's
    // strength, the roughness and the pigment's absorption +/- 30% each, so neighboring
    // strands read apart.
    const float strands[4] = {look.specularRandom + 0.3f * variation, look.roughnessRandom + 0.3f * variation,
                              4.0f * kPi / 180.0f * variation, 0.3f * variation};
    shader.setParameter("hairVariation", strands);
    // Wet hair where the strands carry Wet's wetness (their colors' alpha): core's numbers, soaked.
    shader.setParameter("hairWetLook", wetShadingUniform(arnold.roughness).data());
    shader.setParameter("hairDebug", static_cast<float>(debug));
}

std::vector<MDagPath> ignoredLights(const MDagPath& path) {
    // Maya's light linking as the lightLinker nodes record it: a light ignores an object (or a set,
    // its shading group, holding it) by an ignore entry; a light that does not light everything
    // (not in defaultLightSet) lights only what a link entry names. The groom's own nodes and the
    // sets it belongs to (its instObjGroups' sets). (MLightLinks resolves neither for a plugin
    // shape, and Maya's lightlink takes only the groom's shading group.)
    std::vector<MDagPath> out;
    if (!path.isValid()) return out;
    std::vector<MObject> mine{path.node()};
    MDagPath transform = path;
    if (transform.pop()) mine.push_back(transform.node());
    MFnDependencyNode shape(path.node());
    MPlug groups = shape.findPlug("instObjGroups", false);
    const unsigned groupCount = groups.isNull() ? 0 : groups.evaluateNumElements();
    for (unsigned i = 0; i < groupCount; ++i) {
        MPlugArray sets;
        groups.elementByPhysicalIndex(i).connectedTo(sets, false, true);
        for (unsigned k = 0; k < sets.length(); ++k) mine.push_back(sets[k].node());
    }
    auto isMine = [&](const MObject& node) { return std::any_of(mine.begin(), mine.end(), [&](const MObject& m) { return m == node; }); };
    std::vector<MObject> ignoring, linking;
    auto source = [](const MPlug& plug) {
        MPlugArray from;
        plug.connectedTo(from, true, false);
        return from.length() ? from[0].node() : MObject();
    };
    for (MItDependencyNodes it(MFn::kLightLink); !it.isDone(); it.next()) {
        MFnDependencyNode linker(it.thisNode());
        for (const auto& [array, lightName, objectName] : {std::tuple<const char*, const char*, const char*>{"ignore", "lightIgnored", "objectIgnored"},
                                                           {"link", "light", "object"}}) {
            MPlug entries = linker.findPlug(array, false);
            // (numElements counts what the data block holds: connected elements may be none of it.)
            const unsigned count = entries.isNull() ? 0 : entries.evaluateNumElements();
            for (unsigned i = 0; i < count; ++i) {
                MPlug entry = entries.elementByPhysicalIndex(i);
                MPlug lightPlug, objectPlug;
                for (unsigned c = 0; c < entry.numChildren(); ++c) {
                    const MString name = MFnAttribute(entry.child(c).attribute()).name();
                    if (name == lightName) lightPlug = entry.child(c);
                    if (name == objectName) objectPlug = entry.child(c);
                }
                if (lightPlug.isNull() || objectPlug.isNull()) continue;
                const MObject light = source(lightPlug), object = source(objectPlug);
                if (light.isNull() || object.isNull() || !isMine(object)) continue;
                (std::strcmp(array, "ignore") == 0 ? ignoring : linking).push_back(light);
            }
        }
    }
    // The lights that light everything: defaultLightSet's members (their transforms or shapes).
    std::vector<MObject> everything;
    MSelectionList defaults;
    MObject defaultSet;
    if (defaults.add("defaultLightSet") && defaults.getDependNode(0, defaultSet)) {
        MSelectionList members;
        MFnSet(defaultSet).getMembers(members, false);
        for (unsigned i = 0; i < members.length(); ++i) {
            MDagPath member;
            if (members.getDagPath(i, member)) {
                everything.push_back(member.node());
                if (member.extendToShape()) everything.push_back(member.node());
            }
        }
    }
    auto has = [](const std::vector<MObject>& list, const MObject& node) {
        return std::any_of(list.begin(), list.end(), [&](const MObject& m) { return m == node; });
    };
    for (MItDag it(MItDag::kDepthFirst, MFn::kLight); !it.isDone(); it.next()) {
        MDagPath light;
        if (!it.getPath(light)) continue;
        const MObject node = light.node();
        MDagPath parent = light;
        const MObject parentNode = parent.pop() ? parent.node() : MObject();
        const bool ignores = has(ignoring, node) || (!parentNode.isNull() && has(ignoring, parentNode));
        const bool lightsAll = has(everything, node) || (!parentNode.isNull() && has(everything, parentNode));
        const bool linked = has(linking, node) || (!parentNode.isNull() && has(linking, parentNode));
        if (ignores || (!lightsAll && !linked)) out.push_back(light);
    }
    lastLinking() = {mine.size(), ignoring.size(), linking.size(), everything.size(), out.size()};
    return out;
}
std::string hairLightsJson() {
    const HairDrawStats& stats = drawStats();
    nlohmann::json j;
    j["draws"] = stats.draws;
    j["lightUpdates"] = stats.lightUpdates;
    j["ignoredLights"] = stats.ignored;
    j["linking"] = lastLinking();
    j["count"] = stats.last.count;
    j["ambient"] = {stats.last.ambient[0], stats.last.ambient[1], stats.last.ambient[2]};
    nlohmann::json lights = nlohmann::json::array();
    for (int i = 0; i < stats.last.count; ++i) {
        const HairLight& l = stats.last.lights[i];
        static const char* const kKinds[4] = {"directional", "point", "spot", "area"};
        nlohmann::json one;
        one["type"] = l.type;
        one["kind"] = kKinds[std::clamp(static_cast<int>(l.pos[3] + 0.5f), 0, 3)];
        one["position"] = {l.pos[0], l.pos[1], l.pos[2]};
        one["direction"] = {l.dir[0], l.dir[1], l.dir[2]};
        one["color"] = {l.color[0], l.color[1], l.color[2]};
        one["decay"] = l.color[3];
        one["path"] = l.path.isValid() ? l.path.fullPathName().asChar() : "";
        lights.push_back(one);
    }
    j["lights"] = lights;
    return j.dump();
}

const std::string& lineCoverageText(bool hlsl) {
    static const std::string glsl = kLineCoverageText;
    static const std::string hlslText = hlslOf(kLineCoverageText);
    return hlsl ? hlslText : glsl;
}

RenderingSpace renderingSpace() {
    // Rec.709 to each space and back (Bradford to ACES's white for ACEScg and ACES2065-1).
    static constexpr RenderingSpace kIdentity = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
    static constexpr RenderingSpace kAcesCg = {
        {1.705051f, -0.621792f, -0.083259f, -0.130256f, 1.140805f, -0.010548f, -0.024003f, -0.128969f, 1.152972f},
        {0.613097f, 0.339523f, 0.047379f, 0.070194f, 0.916354f, 0.013452f, 0.020616f, 0.109570f, 0.869815f}};
    static constexpr RenderingSpace kAces2065 = {
        {2.521686f, -1.134131f, -0.387555f, -0.276480f, 1.372719f, -0.096239f, -0.015378f, -0.152975f, 1.168353f},
        {0.439633f, 0.382989f, 0.177378f, 0.089776f, 0.813439f, 0.096784f, 0.017541f, 0.111547f, 0.870912f}};
    static constexpr RenderingSpace kP3 = {
        {1.224940f, -0.224940f, 0.0f, -0.042057f, 1.042057f, 0.0f, -0.019638f, -0.078636f, 1.098274f},
        {0.822462f, 0.177538f, 0.0f, 0.033194f, 0.966806f, 0.0f, 0.017083f, 0.072397f, 0.910520f}};
    static constexpr RenderingSpace kRec2020 = {
        {1.660491f, -0.587641f, -0.072850f, -0.124550f, 1.132900f, -0.008349f, -0.018151f, -0.100579f, 1.118730f},
        {0.627404f, 0.329283f, 0.043313f, 0.069097f, 0.919540f, 0.011362f, 0.016391f, 0.088013f, 0.895595f}};
    int enabled = 0;
    if (!MGlobal::executeCommand("colorManagementPrefs -q -cmEnabled", enabled, false, false) || !enabled) return kIdentity;
    MString name;
    if (!MGlobal::executeCommand("colorManagementPrefs -q -renderingSpaceName", name, false, false)) return kIdentity;
    const std::string space = name.asChar();
    auto has = [&](const char* part) { return space.find(part) != std::string::npos; };
    if (has("ACEScg") || has("AP1")) return kAcesCg;
    if (has("2065") || has("AP0")) return kAces2065;
    if (has("P3")) return kP3;
    if (has("2020")) return kRec2020;
    return kIdentity;
}

}  // namespace groomlab::maya
