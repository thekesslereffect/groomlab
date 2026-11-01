// SPDX-License-Identifier: Apache-2.0
// The draw override's shade fragments: Maya's hair tube shading graph adapted for the cards, the
// strands' own colors, the scales and width as coverage.

#include "groom_draw_override_internal.h"
#include "hair_shading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <maya/MFragmentManager.h>
#include <maya/MGlobal.h>
#include <maya/MViewport2Renderer.h>

namespace groomlab::maya {

namespace {

// The card surface: Maya's own hair tube shading graph (what the strands draw with: diffuse,
// highlights along the strand direction, translucence, the scene's lights, shadows and the
// viewport's post effects such as ambient occlusion), with its color fragment swapped for
// one of ours: the card map (the baked color, sRGB, darkened by the baked AO, with the
// coverage in its alpha: one texture, the graph's own `map`) times the card's tint (its
// vertex color, the fcolor stream: half the ratio of its own strands' color to its shared
// texture's, CardMesh::tints), the pixel cut out where the coverage is below the cutoff (an
// alpha test inside the fragment: hard, depth-correct edges in the opaque pass, nothing to
// sort; the coverage kept as the texture shrinks and hashed below a pixel, so thin strands
// thin out as they do in the strands' drawing instead of vanishing), or, with `solid` on, the
// card color whole (textures off, or nothing baked). The alpha test is off (alphaTest 0) while
// a multisampled color draw turns the coverage into the samples covered instead
// (kCardCoverageFragment, cardCoveragePreDraw); Viewport 2.0 draws the cards' shadow maps with
// the graph too, so their shadows are cut out by it. Implementations in the languages Maya
// composes fragment graphs in: Cg and HLSL (DirectX 11), GLSL (OpenGL Core Profile, whose
// signature leaves the texture out: GLSL's sampler is the texture).
const char* kCardColorFragment = R"x(<fragment uiName="groomLabCardColor" name="groomLabCardColor" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[Card color: the baked color map (sRGB, ambient occlusion in it) cut out by the coverage in its alpha; or a solid color.]]></description>
    <properties>
        <float3 name="color" />
        <float2 name="uvCoord" semantic="uvCoord" flags="varyingInputParam" />
        <int name="tubeDirection" />
        <texture2 name="map" />
        <sampler name="textureSampler" />
        <float name="cutoff" />
        <float name="solid" />
        <float3 name="solidColor" />
        <float name="mapSize" />
        <float name="alphaTest" />
        <float4 name="tint" semantic="fcolor" flags="varyingInputParam" />
        <float3 name="irNormal" />
        <float3 name="irView" />
        <float3 name="iridescenceColor" />
        <float name="iridescenceShift" />
        <texture2 name="irMap" />
        <sampler name="irMapSampler" />
        <float name="cardColorLook" />
        <float3 name="cardFromRec709R" />
        <float3 name="cardFromRec709G" />
        <float3 name="cardFromRec709B" />
    </properties>
    <values>
        <float3 name="color" value="1.0,1.0,1.0" />
        <int name="tubeDirection" value="0" />
        <float name="cutoff" value="0.5" />
        <float name="solid" value="1.0" />
        <float3 name="solidColor" value="0.58,0.50,0.42" />
        <float name="mapSize" value="2048.0" />
        <float name="alphaTest" value="1.0" />
        <float3 name="iridescenceColor" value="0.05,0.55,0.2" />
        <float name="iridescenceShift" value="0.3" />
        <float name="cardColorLook" value="1.0" />
        <float3 name="cardFromRec709R" value="1.0,0.0,0.0" />
        <float3 name="cardFromRec709G" value="0.0,1.0,0.0" />
        <float3 name="cardFromRec709B" value="0.0,0.0,1.0" />
    </values>
    <outputs>
        <float3 name="groomLabCardColor" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabCardColor" />
        <source><![CDATA[
float3 groomLabCardHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabCardColor(float3 color, float2 uv, int tubeDirection, texture2D map, sampler2D textureSampler,
                         float cutoff, float solid, float3 solidColor, float mapSize, float alphaTest, float4 tint,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift, texture2D irMap,
                         sampler2D irMapSampler, float cardColorLook, float3 cardFromRec709R, float3 cardFromRec709G,
                         float3 cardFromRec709B)
{
    if (solid > 0.5f) return solidColor;
    float4 m = tex2D(textureSampler, uv);
    // Coverage kept as the texture shrinks: each mip level down averages thin strands with the
    // gaps beside them, so a quarter more a level; magnified, cut at the cutoff (crisp edges);
    // a texel or more to a pixel, at a threshold hashed per pixel (in uv), so a strand thinner
    // than a pixel shows as often as it covers it instead of not at all.
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float texels = max(footprint.x, footprint.y) * mapSize;
    float lod = max(log2(max(texels, 1e-6f)), 0.0f);
    float2 cell = floor(uv / footprint);
    float noise = frac(sin(dot(cell, float2(12.9898f, 78.233f))) * 43758.5453f);
    if (alphaTest > 0.5f) clip(m.a * (1.0f + 0.25f * lod) - lerp(cutoff, noise, saturate(lod)));
    float3 c = m.rgb;
    c = lerp(c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f), step(0.04045f, c));
    // The map's Rec.709 to the rendering space (as a file node reads it), then the hair's
    // appearance color, what Maya's hair graph shades (hair_color.h): the bake's absorption, a
    // Shaded bake's (hairBaseOfShaded) or the base color's (hairColorAbsorption, roughness 0.2).
    c = float3(dot(cardFromRec709R, c), dot(cardFromRec709G, c), dot(cardFromRec709B, c));
    float3 sigma;
    if (cardColorLook < 0.5f) {
        float3 f = 0.68686f * c + 0.10438f * (c.x + c.y + c.z);
        float3 share = clamp((f / 0.7079f - 0.0989f) / 0.9011f, 1e-6f, 1.0f);
        sigma = pow(-log(share) / 4.5472f, float3(1.468f, 1.468f, 1.468f));
    } else {
        float3 l = log(clamp(c, 1e-8f, 1.0f)) / 5.9504f;
        sigma = l * l;
    }
    c = 0.03f + 0.97f * exp(-3.52f * pow(sigma, float3(0.79f, 0.79f, 0.79f)));
    // The card's own color: its tint (vertex color, 0.5 none) over the shared texture's.
    float3 result = color * c * 2.0f * tint.rgb;
    // Structural color: how much, the bake's (the Color map's alpha, per texel: one vane of a
    // feather, say) where the card's own strands have any (its vertex color's alpha past 1); its
    // hue turns as the view leaves the card's normal (a feather's vane) (color_patterns.h).
    float amount = saturate((tint.a - 1.0f) * 4.0f) * tex2D(irMapSampler, uv).r;
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabCardHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabCardColor" />
        <source><![CDATA[
float3 groomLabCardHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabCardColor(float3 color, float2 uv, int tubeDirection, Texture2D map, sampler textureSampler,
                         float cutoff, float solid, float3 solidColor, float mapSize, float alphaTest, float4 tint,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift, Texture2D irMap,
                         sampler irMapSampler, float cardColorLook, float3 cardFromRec709R, float3 cardFromRec709G,
                         float3 cardFromRec709B)
{
    if (solid > 0.5f) return solidColor;
    float4 m = map.Sample(textureSampler, uv);
    // Coverage kept as the texture shrinks: each mip level down averages thin strands with the
    // gaps beside them, so a quarter more a level; magnified, cut at the cutoff (crisp edges);
    // a texel or more to a pixel, at a threshold hashed per pixel (in uv), so a strand thinner
    // than a pixel shows as often as it covers it instead of not at all.
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float texels = max(footprint.x, footprint.y) * mapSize;
    float lod = max(log2(max(texels, 1e-6f)), 0.0f);
    float2 cell = floor(uv / footprint);
    float noise = frac(sin(dot(cell, float2(12.9898f, 78.233f))) * 43758.5453f);
    if (alphaTest > 0.5f) clip(m.a * (1.0f + 0.25f * lod) - lerp(cutoff, noise, saturate(lod)));
    float3 c = m.rgb;
    c = lerp(c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f), step(0.04045f, c));
    // The map's Rec.709 to the rendering space (as a file node reads it), then the hair's
    // appearance color, what Maya's hair graph shades (hair_color.h): the bake's absorption, a
    // Shaded bake's (hairBaseOfShaded) or the base color's (hairColorAbsorption, roughness 0.2).
    c = float3(dot(cardFromRec709R, c), dot(cardFromRec709G, c), dot(cardFromRec709B, c));
    float3 sigma;
    if (cardColorLook < 0.5f) {
        float3 f = 0.68686f * c + 0.10438f * (c.x + c.y + c.z);
        float3 share = clamp((f / 0.7079f - 0.0989f) / 0.9011f, 1e-6f, 1.0f);
        sigma = pow(-log(share) / 4.5472f, float3(1.468f, 1.468f, 1.468f));
    } else {
        float3 l = log(clamp(c, 1e-8f, 1.0f)) / 5.9504f;
        sigma = l * l;
    }
    c = 0.03f + 0.97f * exp(-3.52f * pow(sigma, float3(0.79f, 0.79f, 0.79f)));
    // The card's own color: its tint (vertex color, 0.5 none) over the shared texture's.
    float3 result = color * c * 2.0f * tint.rgb;
    // Structural color: how much, the bake's (the Color map's alpha, per texel: one vane of a
    // feather, say) where the card's own strands have any (its vertex color's alpha past 1); its
    // hue turns as the view leaves the card's normal (a feather's vane) (color_patterns.h).
    float amount = saturate((tint.a - 1.0f) * 4.0f) * irMap.Sample(irMapSampler, uv).r;
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabCardHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabCardColor" />
        <source><![CDATA[
vec3 groomLabCardHue(vec3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0;
    if (d > 1e-8) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0 + (c.b - c.r) / d;
        else h = 4.0 + (c.r - c.g) / d;
        h /= 6.0;
    }
    float s = hi > 1e-8 ? d / hi : 0.0;
    h = fract(h + turns);
    vec3 k = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return hi * (1.0 - s + s * k);
}
vec3 groomLabCardColor(vec3 color, vec2 uv, int tubeDirection, sampler2D textureSampler, float cutoff, float solid,
                       vec3 solidColor, float mapSize, float alphaTest, vec4 tint,
                       vec3 irNormal, vec3 irView, vec3 iridescenceColor, float iridescenceShift, sampler2D irMapSampler,
                       float cardColorLook, vec3 cardFromRec709R, vec3 cardFromRec709G, vec3 cardFromRec709B)
{
    if (solid > 0.5) return solidColor;
    vec4 m = texture(textureSampler, uv);
    vec2 footprint = max(fwidth(uv), vec2(1e-7));
    float texels = max(footprint.x, footprint.y) * mapSize;
    float lod = max(log2(max(texels, 1e-6)), 0.0);
    vec2 cell = floor(uv / footprint);
    float noise = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    if (alphaTest > 0.5 && m.a * (1.0 + 0.25 * lod) - mix(cutoff, noise, clamp(lod, 0.0, 1.0)) < 0.0) discard;
    vec3 c = m.rgb;
    c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    c = vec3(dot(cardFromRec709R, c), dot(cardFromRec709G, c), dot(cardFromRec709B, c));
    vec3 sigma;
    if (cardColorLook < 0.5) {
        vec3 f = 0.68686 * c + 0.10438 * (c.x + c.y + c.z);
        vec3 share = clamp((f / 0.7079 - 0.0989) / 0.9011, 1e-6, 1.0);
        sigma = pow(-log(share) / 4.5472, vec3(1.468, 1.468, 1.468));
    } else {
        vec3 l = log(clamp(c, 1e-8, 1.0)) / 5.9504;
        sigma = l * l;
    }
    c = 0.03 + 0.97 * exp(-3.52 * pow(sigma, vec3(0.79, 0.79, 0.79)));
    vec3 result = color * c * 2.0 * tint.rgb;
    float amount = clamp((tint.a - 1.0) * 4.0, 0.0, 1.0) * texture(irMapSampler, uv).r;
    if (amount > 0.0) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = mix(result, groomLabCardHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabCardColor" />
        <source><![CDATA[
float3 groomLabCardHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabCardColor(float3 color, float2 uv, int tubeDirection, Texture2D map, sampler textureSampler,
                         float cutoff, float solid, float3 solidColor, float mapSize, float alphaTest, float4 tint,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift, Texture2D irMap,
                         sampler irMapSampler, float cardColorLook, float3 cardFromRec709R, float3 cardFromRec709G,
                         float3 cardFromRec709B)
{
    if (solid > 0.5f) return solidColor;
    float4 m = map.Sample(textureSampler, uv);
    // Coverage kept as the texture shrinks: each mip level down averages thin strands with the
    // gaps beside them, so a quarter more a level; magnified, cut at the cutoff (crisp edges);
    // a texel or more to a pixel, at a threshold hashed per pixel (in uv), so a strand thinner
    // than a pixel shows as often as it covers it instead of not at all.
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float texels = max(footprint.x, footprint.y) * mapSize;
    float lod = max(log2(max(texels, 1e-6f)), 0.0f);
    float2 cell = floor(uv / footprint);
    float noise = frac(sin(dot(cell, float2(12.9898f, 78.233f))) * 43758.5453f);
    if (alphaTest > 0.5f) clip(m.a * (1.0f + 0.25f * lod) - lerp(cutoff, noise, saturate(lod)));
    float3 c = m.rgb;
    c = lerp(c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f), step(0.04045f, c));
    // The map's Rec.709 to the rendering space (as a file node reads it), then the hair's
    // appearance color, what Maya's hair graph shades (hair_color.h): the bake's absorption, a
    // Shaded bake's (hairBaseOfShaded) or the base color's (hairColorAbsorption, roughness 0.2).
    c = float3(dot(cardFromRec709R, c), dot(cardFromRec709G, c), dot(cardFromRec709B, c));
    float3 sigma;
    if (cardColorLook < 0.5f) {
        float3 f = 0.68686f * c + 0.10438f * (c.x + c.y + c.z);
        float3 share = clamp((f / 0.7079f - 0.0989f) / 0.9011f, 1e-6f, 1.0f);
        sigma = pow(-log(share) / 4.5472f, float3(1.468f, 1.468f, 1.468f));
    } else {
        float3 l = log(clamp(c, 1e-8f, 1.0f)) / 5.9504f;
        sigma = l * l;
    }
    c = 0.03f + 0.97f * exp(-3.52f * pow(sigma, float3(0.79f, 0.79f, 0.79f)));
    // The card's own color: its tint (vertex color, 0.5 none) over the shared texture's.
    float3 result = color * c * 2.0f * tint.rgb;
    // Structural color: how much, the bake's (the Color map's alpha, per texel: one vane of a
    // feather, say) where the card's own strands have any (its vertex color's alpha past 1); its
    // hue turns as the view leaves the card's normal (a feather's vane) (color_patterns.h).
    float amount = saturate((tint.a - 1.0f) * 4.0f) * irMap.Sample(irMapSampler, uv).r;
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabCardHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// The card's alpha to coverage, after the card graph's last struct (mayaComputeSurfaceFinal's: the
// color and its alpha): with alphaToCoverage on (cardCoveragePreDraw: a multisampled color draw,
// its blend state turning the alpha into the share of the pixel's samples covered; the color
// fragment's alpha test off), the alpha is the card map's coverage at the uvs (the same texture as
// the color fragment's map, bound twice: coverageMap; the uvs through a pass-through feeding both):
// magnified, a ramp a pixel wide across the cutoff, so the samples smooth the edge; a texel or more
// to a pixel, the coverage kept as the texture shrinks (each mip level down averages thin strands
// with the gaps beside them, so a quarter more a level). Off, the struct as it is. (A fragment of
// the graph cannot write the coverage mask itself: kStrandCoverageFragment's notes.) The
// derivatives are taken before any branch. coverageSolid: the solid cards (nothing to cut).
const char* kCardCoverageFragment = R"x(<fragment uiName="groomLabCardCoverage" name="groomLabCardCoverage" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[Card coverage: the card map's alpha as the pixel's alpha, for alpha to coverage.]]></description>
    <properties>
        <struct name="input" struct_name="mayaSurfaceShaderOutput" />
        <float2 name="uv" />
        <texture2 name="coverageMap" />
        <sampler name="coverageMapSampler" />
        <float name="cutoff" />
        <float name="mapSize" />
        <float name="coverageSolid" />
        <float name="alphaToCoverage" />
    </properties>
    <values>
        <float name="cutoff" value="0.5" />
        <float name="mapSize" value="2048.0" />
        <float name="coverageSolid" value="1.0" />
        <float name="alphaToCoverage" value="0.0" />
    </values>
    <outputs>
        <struct name="groomLabCardCoverage" struct_name="mayaSurfaceShaderOutput" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabCardCoverage" />
        <source><![CDATA[
mayaSurfaceShaderOutput groomLabCardCoverage(mayaSurfaceShaderOutput input, float2 uv, texture2D coverageMap,
                                             sampler2D coverageMapSampler, float cutoff, float mapSize, float coverageSolid,
                                             float alphaToCoverage)
{
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6f)), 0.0f);
    float kept = tex2D(coverageMapSampler, uv).a * (1.0f + 0.25f * lod);
    float ramp = max(fwidth(kept), 1e-4f);
    if (coverageSolid > 0.5f || alphaToCoverage < 0.5f) return input;
    float alpha = lerp(saturate((kept - cutoff) / ramp + 0.5f), saturate(kept), saturate(lod));
    clip(alpha - 1e-3f);
    input.outSurfaceFinal.a = alpha;
    return input;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabCardCoverage" />
        <source><![CDATA[
mayaSurfaceShaderOutput groomLabCardCoverage(mayaSurfaceShaderOutput input, float2 uv, Texture2D coverageMap,
                                             sampler coverageMapSampler, float cutoff, float mapSize, float coverageSolid,
                                             float alphaToCoverage)
{
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6f)), 0.0f);
    float kept = coverageMap.Sample(coverageMapSampler, uv).a * (1.0f + 0.25f * lod);
    float ramp = max(fwidth(kept), 1e-4f);
    if (coverageSolid > 0.5f || alphaToCoverage < 0.5f) return input;
    float alpha = lerp(saturate((kept - cutoff) / ramp + 0.5f), saturate(kept), saturate(lod));
    clip(alpha - 1e-3f);
    input.outSurfaceFinal.a = alpha;
    return input;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabCardCoverage" />
        <source><![CDATA[
mayaSurfaceShaderOutput groomLabCardCoverage(mayaSurfaceShaderOutput surface, vec2 uv, sampler2D coverageMapSampler,
                                             float cutoff, float mapSize, float coverageSolid, float alphaToCoverage)
{
    vec2 footprint = max(fwidth(uv), vec2(1e-7));
    float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6)), 0.0);
    float kept = texture(coverageMapSampler, uv).a * (1.0 + 0.25 * lod);
    float ramp = max(fwidth(kept), 1e-4);
    if (coverageSolid > 0.5 || alphaToCoverage < 0.5) return surface;
    float alpha = mix(clamp((kept - cutoff) / ramp + 0.5, 0.0, 1.0), clamp(kept, 0.0, 1.0), clamp(lod, 0.0, 1.0));
    if (alpha < 1e-3) discard;
    surface.outSurfaceFinal.a = alpha;
    return surface;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabCardCoverage" />
        <source><![CDATA[
mayaSurfaceShaderOutput groomLabCardCoverage(mayaSurfaceShaderOutput input, float2 uv, Texture2D coverageMap,
                                             sampler coverageMapSampler, float cutoff, float mapSize, float coverageSolid,
                                             float alphaToCoverage)
{
    float2 footprint = max(fwidth(uv), float2(1e-7f, 1e-7f));
    float lod = max(log2(max(max(footprint.x, footprint.y) * mapSize, 1e-6f)), 0.0f);
    float kept = coverageMap.Sample(coverageMapSampler, uv).a * (1.0f + 0.25f * lod);
    float ramp = max(fwidth(kept), 1e-4f);
    if (coverageSolid > 0.5f || alphaToCoverage < 0.5f) return input;
    float alpha = lerp(saturate((kept - cutoff) / ramp + 0.5f), saturate(kept), saturate(lod));
    clip(alpha - 1e-3f);
    input.outSurfaceFinal.a = alpha;
    return input;
}
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// The card surface's geometry terms (N.L, N.V, N.H, V.H, as Maya's mayaShaderGeom has them) from
// the card's shading normal carried in its bitangent stream (cardGeometry), not from Nw: with
// two-sided lighting on, Viewport 2.0 turns Nw round on a face seen from behind, so a card's back
// lit the other way from the strands it stands for (lines, which have no back), and hair hanging
// behind the head showed bright through the gap under it.
const char* kCardGeomFragment = R"x(<fragment uiName="groomLabCardShaderGeom" name="groomLabCardShaderGeom" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[Shader geometry terms from the card's shading normal (its bitangent stream), the same from either side.]]></description>
    <properties>
        <float3 name="Nw" flags="varyingInputParam" />
        <float3 name="Lw" />
        <float3 name="Vw" flags="varyingInputParam" />
        <float3 name="Hw" />
        <float3 name="cardNormal" semantic="bitangent" flags="varyingInputParam" />
    </properties>
    <values>
        <float3 name="Lw" value="0.0,0.0,0.0" />
    </values>
    <outputs>
        <float4 name="groomLabCardShaderGeom" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabCardShaderGeom" />
        <source><![CDATA[
float4 groomLabCardShaderGeom(float3 Nw, float3 L, float3 V, float3 H, float3 cardNormal)
{
    float3 N = normalize(cardNormal);
    return float4(saturate(dot(N, L)), saturate(dot(N, V)), saturate(dot(N, H)), dot(V, H));
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabCardShaderGeom" />
        <source><![CDATA[
float4 groomLabCardShaderGeom(float3 Nw, float3 L, float3 V, float3 H, float3 cardNormal)
{
    float3 N = normalize(cardNormal);
    return float4(saturate(dot(N, L)), saturate(dot(N, V)), saturate(dot(N, H)), dot(V, H));
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabCardShaderGeom" />
        <source><![CDATA[
vec4 groomLabCardShaderGeom(vec3 Nw, vec3 L, vec3 V, vec3 H, vec3 cardNormal)
{
    vec3 N = normalize(cardNormal);
    return vec4(clamp(dot(N, L), 0.0, 1.0), clamp(dot(N, V), 0.0, 1.0), clamp(dot(N, H), 0.0, 1.0), dot(V, H));
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabCardShaderGeom" />
        <source><![CDATA[
float4 groomLabCardShaderGeom(float3 Nw, float3 L, float3 V, float3 H, float3 cardNormal)
{
    float3 N = normalize(cardNormal);
    return float4(saturate(dot(N, L)), saturate(dot(N, V)), saturate(dot(N, H)), dot(V, H));
}
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// The lit strands in their own colors: Maya's hair tube shading graph (what the strands draw
// with otherwise: the same lights, highlights, translucence, shadows and post effects), with its
// color fragment swapped for this one: Maya's own (the color times the root-to-tip ramp at u)
// times the vertex's color, the colors stream (semantic fcolor: Viewport 2.0 asks the item for
// its kColor stream, RGBA floats: given RGB floats or RGBA bytes instead, Maya 2022 drew nothing
// or hung, on both APIs). In every language Maya composes fragment graphs in, as its own fragments
// carry them: Cg (its DirectX 11 path takes it), HLSL, and GLSL for the OpenGL Core Profile
// (whose signature leaves the texture out: GLSL's sampler is the texture).
const char* kHairColorFragment = R"x(<fragment uiName="groomLabHairColor" name="groomLabHairColor" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[Hair tube color times the vertex color: each strand's own color, lit by Maya's hair tube shading.]]></description>
    <properties>
        <float3 name="color" />
        <float2 name="uvCoord" semantic="uvCoord" flags="varyingInputParam" />
        <int name="tubeDirection" />
        <texture2 name="map" />
        <sampler name="textureSampler" />
        <float4 name="strandColor" semantic="fcolor" flags="varyingInputParam" />
        <float3 name="irNormal" />
        <float3 name="irView" />
        <float3 name="iridescenceColor" />
        <float name="iridescenceShift" />
    </properties>
    <values>
        <float3 name="color" value="1.0,1.0,1.0" />
        <int name="tubeDirection" value="0" />
        <float3 name="iridescenceColor" value="0.05,0.55,0.2" />
        <float name="iridescenceShift" value="0.3" />
    </values>
    <outputs>
        <float3 name="groomLabHairColor" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabHairColor" />
        <source><![CDATA[
float3 groomLabHairHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabHairColor(float3 color, float2 uv, int tubeDirection, texture2D map, sampler2D textureSampler, float4 strandColor,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift)
{
    float2 lookup = float2(tubeDirection == 0 ? uv.x : uv.y, 0.5f);
    float3 result = color * strandColor.rgb * tex2D(textureSampler, lookup).rgb;
    // Structural color: the vertex color's alpha past 1 says how much (FeatherColor's iridescence);
    // its hue turns as the view leaves the strand's normal (a feather's vane) (color_patterns.h).
    float amount = saturate(strandColor.a - 1.0f);
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabHairHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabHairColor" />
        <source><![CDATA[
float3 groomLabHairHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabHairColor(float3 color, float2 uv, int tubeDirection, Texture2D map, sampler textureSampler, float4 strandColor,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift)
{
    float2 lookup = float2(tubeDirection == 0 ? uv.x : uv.y, 0.5f);
    float3 result = color * strandColor.rgb * map.Sample(textureSampler, lookup).rgb;
    // Structural color: the vertex color's alpha past 1 says how much (FeatherColor's iridescence);
    // its hue turns as the view leaves the strand's normal (a feather's vane) (color_patterns.h).
    float amount = saturate(strandColor.a - 1.0f);
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabHairHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabHairColor" />
        <source><![CDATA[
vec3 groomLabHairHue(vec3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0;
    if (d > 1e-8) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0 + (c.b - c.r) / d;
        else h = 4.0 + (c.r - c.g) / d;
        h /= 6.0;
    }
    float s = hi > 1e-8 ? d / hi : 0.0;
    h = fract(h + turns);
    vec3 k = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return hi * (1.0 - s + s * k);
}
vec3 groomLabHairColor(vec3 color, vec2 uv, int tubeDirection, sampler2D textureSampler, vec4 strandColor,
                       vec3 irNormal, vec3 irView, vec3 iridescenceColor, float iridescenceShift)
{
    vec2 lookup = vec2(tubeDirection == 0 ? uv.x : uv.y, 0.5f);
    vec3 result = color * strandColor.rgb * texture(textureSampler, lookup).rgb;
    float amount = clamp(strandColor.a - 1.0, 0.0, 1.0);
    if (amount > 0.0) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = mix(result, groomLabHairHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabHairColor" />
        <source><![CDATA[
float3 groomLabHairHue(float3 c, float turns)
{
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0f;
    if (d > 1e-8f) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0f + (c.b - c.r) / d;
        else h = 4.0f + (c.r - c.g) / d;
        h /= 6.0f;
    }
    float s = hi > 1e-8f ? d / hi : 0.0f;
    h = frac(h + turns);
    float3 k = saturate(abs(fmod(h * 6.0f + float3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f);
    return hi * (1.0f - s + s * k);
}
float3 groomLabHairColor(float3 color, float2 uv, int tubeDirection, Texture2D map, sampler textureSampler, float4 strandColor,
                         float3 irNormal, float3 irView, float3 iridescenceColor, float iridescenceShift)
{
    float2 lookup = float2(tubeDirection == 0 ? uv.x : uv.y, 0.5f);
    float3 result = color * strandColor.rgb * map.Sample(textureSampler, lookup).rgb;
    // Structural color: the vertex color's alpha past 1 says how much (FeatherColor's iridescence);
    // its hue turns as the view leaves the strand's normal (a feather's vane) (color_patterns.h).
    float amount = saturate(strandColor.a - 1.0f);
    if (amount > 0.0f) {
        float grazing = 1.0 - abs(dot(normalize(irView), normalize(irNormal)));
        result = lerp(result, groomLabHairHue(iridescenceColor, iridescenceShift * grazing), amount);
    }
    return result;
}
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// The coverage of the lit lines (hashed: line_coverage.glsl, display.h lineHashThreshold, as the
// flat lines' effect draws it): after the hair graph's last struct (mayaComputeSurfaceFinal's: the
// color and its alpha), the fragment kept where the share of the coverage kept (lineKeepShare of
// the coverage from the uvs' width, the world position, the projection and the viewport, at the
// render's density: lineDensity) reaches the threshold hashed from the uvs' strand number and u, its
// alpha times coverage / share (lineCoverage 0: off, every fragment kept as it is); on multisampled
// targets too: a fragment of the graph cannot write the coverage mask (DirectX 11's has no
// SV_Coverage of its own; the GLSL Maya 2022 composes is older than gl_SampleMask's), so the
// fragment is kept or not as a whole, its samples the line's. lineSamples is the target's samples a
// pixel (coverageSamplesPreDraw): one, stretches about four pixels long (lineThreshold). The threshold's
// derivatives are taken before any branch. The HLSL and GLSL implementations take
// line_coverage.glsl where @LINE_COVERAGE@ stands (strandCoverageFragment); Cg (no unsigned integers
// there) hashes with floats. GLSL names the struct's argument otherwise (`input` is reserved there).
const char* kStrandCoverageFragment = R"x(<fragment uiName="groomLabStrandCoverage" name="groomLabStrandCoverage" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[Strand lines' width as coverage: the fragment kept where the share of a pixel the strand's width covers reaches a threshold hashed along the strand.]]></description>
    <properties>
        <struct name="input" struct_name="mayaSurfaceShaderOutput" />
        <float2 name="uv" />
        <float3 name="Pw" />
        <float4x4 name="viewProj" semantic="viewprojection" />
        <float4x4 name="projection" semantic="projection" />
        <float2 name="viewport" semantic="viewportPixelSize" />
        <float name="lineCoverage" />
        <float name="lineWidthScale" />
        <float name="lineCoverageFloor" />
        <float name="lineDensity" />
        <float name="lineLod" />
        <float name="lineBlend" />
        <float name="lineSamples" />
    </properties>
    <values>
        <float name="lineCoverage" value="0.0" />
        <float name="lineWidthScale" value="1.0" />
        <float name="lineCoverageFloor" value="0.0" />
        <float name="lineDensity" value="1.0" />
        <float name="lineLod" value="0.0" />
        <float name="lineBlend" value="0.0" />
        <float name="lineSamples" value="1.0" />
    </values>
    <outputs>
        <struct name="groomLabStrandCoverage" struct_name="mayaSurfaceShaderOutput" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabStrandCoverage" />
        <source><![CDATA[
float groomLabLineStretch(float strand, float u, float level)
{
    float3 p3 = frac(float3(strand, floor(u * exp2(level)), level + 64.0f) * 0.1031f);
    p3 += dot(p3, p3.zyx + 31.32f);
    return frac((p3.x + p3.y) * p3.z);
}
float groomLabLineThreshold(float encoded, float samples)
{
    float perPixel = max(length(float2(ddx(encoded), ddy(encoded))), 1e-7f);
    float strand = max(floor(0.5f * encoded), 0.0f);
    float u = encoded - 2.0f * strand;
    // One sample a pixel: stretches of about four pixels (line_coverage.glsl lineThreshold).
    float level = log2(1.0f / perPixel) - (samples <= 1.5f ? 2.0f : 0.0f);
    float lo = floor(level), f = level - lo;
    float x = (1.0f - f) * groomLabLineStretch(strand, u, lo) + f * groomLabLineStretch(strand, u, lo + 1.0f);
    float a = min(f, 1.0f - f);
    float t = x;
    if (a > 1e-4f) {
        if (x < a) t = x * x / (2.0f * a * (1.0f - a));
        else if (x < 1.0f - a) t = (x - 0.5f * a) / (1.0f - a);
        else t = 1.0f - (1.0f - x) * (1.0f - x) / (2.0f * a * (1.0f - a));
    }
    return clamp(t, 1e-6f, 1.0f);
}
mayaSurfaceShaderOutput groomLabStrandCoverage(mayaSurfaceShaderOutput input, float2 uv, float3 Pw, float4x4 viewProj,
                                               float4x4 projection, float2 viewport, float lineCoverage, float lineWidthScale,
                                               float lineCoverageFloor, float lineDensity, float lineLod,
                                               float lineBlend, float lineSamples)
{
    float t = groomLabLineThreshold(uv.x, lineSamples);
    bool opaque = true;
    if (lineCoverage > 0.5f && uv.y >= 0.0f) {
        float w = mul(float4(Pw, 1.0f), viewProj).w;
        if (w > 0.0f) {
            float own = clamp(uv.y * lineWidthScale * 0.5f * projection[1][1] * viewport.y / w, lineCoverageFloor, 1.0f);
            float exponent = (lineLod > 0.0f ? lineLod : 1.0f) / (lineDensity > 0.0f && lineDensity < 1.0f ? lineDensity : 1.0f);
            float c = exponent != 1.0f && own < 1.0f ? 1.0f - pow(1.0f - own, exponent) : own;
            float keep = c + (1.0f - c) * smoothstep(0.1f, 0.4f, lineLod > 0.0f ? 0.0f : own);
            if (keep < t) discard;
            input.outSurfaceFinal.a *= c / keep;
            opaque = c >= 0.9999f * keep;
        }
    }
    // Of two passes (the level of detail's blend item): 1 the opaque fragments alone, 2 the blended.
    if (lineBlend > 1.5f ? opaque : (lineBlend > 0.5f && !opaque)) discard;
    return input;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabStrandCoverage" />
        <source><![CDATA[
@LINE_COVERAGE@
mayaSurfaceShaderOutput groomLabStrandCoverage(mayaSurfaceShaderOutput input, float2 uv, float3 Pw, float4x4 viewProj,
                                               float4x4 projection, float2 viewport, float lineCoverage, float lineWidthScale,
                                               float lineCoverageFloor, float lineDensity, float lineLod,
                                               float lineBlend, float lineSamples)
{
    float perPixel = length(float2(ddx(uv.x), ddy(uv.x)));
    float number = floor(0.5f * uv.x);
    uint strand = uint(max(number, 0.0f));
    float t = lineThreshold(strand, uv.x - 2.0f * number, perPixel, uint(max(lineSamples, 1.0f)));
    bool opaque = true;
    if (lineCoverage > 0.5f && uv.y >= 0.0f) {
        float w = mul(float4(Pw, 1.0f), viewProj).w;
        if (w > 0.0f) {
            float2 c = lineCoverageOf(uv.y, w, lineWidthScale, 0.5f * projection[1][1] * viewport.y, lineCoverageFloor, lineDensity, lineLod);
            float keep = lineKeepShare(c.x, c.y);
            if (keep < t) discard;
            input.outSurfaceFinal.a *= c.x / keep;
            opaque = c.x >= 0.9999f * keep;
        }
    }
    // Of two passes (the level of detail's blend item): 1 the opaque fragments alone, 2 the blended.
    if (lineBlend > 1.5f ? opaque : (lineBlend > 0.5f && !opaque)) discard;
    return input;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabStrandCoverage" />
        <source><![CDATA[
@LINE_COVERAGE@
mayaSurfaceShaderOutput groomLabStrandCoverage(mayaSurfaceShaderOutput surface, vec2 uv, vec3 Pw, mat4 viewProj,
                                               mat4 projection, vec2 viewport, float lineCoverage, float lineWidthScale,
                                               float lineCoverageFloor, float lineDensity, float lineLod,
                                               float lineBlend, float lineSamples)
{
    float perPixel = length(vec2(dFdx(uv.x), dFdy(uv.x)));
    float number = floor(0.5 * uv.x);
    uint strand = uint(max(number, 0.0));
    float t = lineThreshold(strand, uv.x - 2.0 * number, perPixel, uint(max(lineSamples, 1.0)));
    bool opaque = true;
    if (lineCoverage > 0.5 && uv.y >= 0.0) {
        float w = (viewProj * vec4(Pw, 1.0)).w;
        if (w > 0.0) {
            vec2 c = lineCoverageOf(uv.y, w, lineWidthScale, 0.5 * projection[1][1] * viewport.y, lineCoverageFloor, lineDensity, lineLod);
            float keep = lineKeepShare(c.x, c.y);
            if (keep < t) discard;
            surface.outSurfaceFinal.a *= c.x / keep;
            opaque = c.x >= 0.9999 * keep;
        }
    }
    // Of two passes (the level of detail's blend item): 1 the opaque fragments alone, 2 the blended.
    if (lineBlend > 1.5 ? opaque : (lineBlend > 0.5 && !opaque)) discard;
    return surface;
}
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabStrandCoverage" />
        <source><![CDATA[
@LINE_COVERAGE@
mayaSurfaceShaderOutput groomLabStrandCoverage(mayaSurfaceShaderOutput input, float2 uv, float3 Pw, float4x4 viewProj,
                                               float4x4 projection, float2 viewport, float lineCoverage, float lineWidthScale,
                                               float lineCoverageFloor, float lineDensity, float lineLod,
                                               float lineBlend, float lineSamples)
{
    float perPixel = length(float2(ddx(uv.x), ddy(uv.x)));
    float number = floor(0.5f * uv.x);
    uint strand = uint(max(number, 0.0f));
    float t = lineThreshold(strand, uv.x - 2.0f * number, perPixel, uint(max(lineSamples, 1.0f)));
    bool opaque = true;
    if (lineCoverage > 0.5f && uv.y >= 0.0f) {
        float w = mul(float4(Pw, 1.0f), viewProj).w;
        if (w > 0.0f) {
            float2 c = lineCoverageOf(uv.y, w, lineWidthScale, 0.5f * projection[1][1] * viewport.y, lineCoverageFloor, lineDensity, lineLod);
            float keep = lineKeepShare(c.x, c.y);
            if (keep < t) discard;
            input.outSurfaceFinal.a *= c.x / keep;
            opaque = c.x >= 0.9999f * keep;
        }
    }
    // Of two passes (the level of detail's blend item): 1 the opaque fragments alone, 2 the blended.
    if (lineBlend > 1.5f ? opaque : (lineBlend > 0.5f && !opaque)) discard;
    return input;
}
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// kStrandCoverageFragment with line_coverage.glsl in each implementation's words (@LINE_COVERAGE@):
// HLSL's for the HLSL implementations, GLSL's for the GLSL one.
const std::string& strandCoverageFragment() {
    static const std::string xml = [] {
        std::string text = kStrandCoverageFragment;
        const std::string token = "@LINE_COVERAGE@";
        for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at)) {
            const std::size_t language = text.rfind("language=\"", at);
            const bool hlsl = language != std::string::npos && text.compare(language, 14, "language=\"HLSL") == 0;
            const std::string& shared = lineCoverageText(hlsl);
            text.replace(at, token.size(), shared);
            at += shared.size();
        }
        return text;
    }();
    return xml;
}

// The lines' uvs stream (uvCoord) handed to the fragments reading it, the color's and the coverage's:
// a pass-through whose own input is the stream (one property of the semantic: one stream asked for;
// Maya's float2 pass-through, whose input has no semantic, left the stream out of the shader).
const char* kUvPassThroughFragment = R"x(<fragment uiName="groomLabUvPassThrough" name="groomLabUvPassThrough" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[The uvCoord stream, passed through.]]></description>
    <properties>
        <float2 name="uvCoord" semantic="uvCoord" flags="varyingInputParam" />
    </properties>
    <values>
    </values>
    <outputs>
        <float2 name="groomLabUvPassThrough" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabUvPassThrough" />
        <source><![CDATA[
float2 groomLabUvPassThrough(float2 uv) { return uv; }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabUvPassThrough" />
        <source><![CDATA[
float2 groomLabUvPassThrough(float2 uv) { return uv; }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabUvPassThrough" />
        <source><![CDATA[
vec2 groomLabUvPassThrough(vec2 uv) { return uv; }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabUvPassThrough" />
        <source><![CDATA[
float2 groomLabUvPassThrough(float2 uv) { return uv; }
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

// The lines' uvs as the color fragment reads them: u alone, the strand's number (which the
// coverage reads) taken off (display.h lineUvU; the ribbons' uvs carry none: as they are).
const char* kLineUvFragment = R"x(<fragment uiName="groomLabLineUv" name="groomLabLineUv" type="plumbing" class="ShadeFragment" version="1.0" feature_level="0">
    <description><![CDATA[The strand lines' uvs with u alone (the strand's number taken off).]]></description>
    <properties>
        <float2 name="uv" />
    </properties>
    <values>
    </values>
    <outputs>
        <float2 name="groomLabLineUv" />
    </outputs>
    <implementation>
    <implementation render="OGSRenderer" language="Cg" lang_version="2.1">
        <function_name val="groomLabLineUv" />
        <source><![CDATA[
float2 groomLabLineUv(float2 uv) { return float2(uv.x - 2.0f * floor(0.5f * uv.x), uv.y); }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="11.0">
        <function_name val="groomLabLineUv" />
        <source><![CDATA[
float2 groomLabLineUv(float2 uv) { return float2(uv.x - 2.0f * floor(0.5f * uv.x), uv.y); }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="GLSL" lang_version="3.0">
        <function_name val="groomLabLineUv" />
        <source><![CDATA[
vec2 groomLabLineUv(vec2 uv) { return vec2(uv.x - 2.0 * floor(0.5 * uv.x), uv.y); }
]]></source>
    </implementation>
    <implementation render="OGSRenderer" language="HLSL" lang_version="10.0">
        <function_name val="groomLabLineUv" />
        <source><![CDATA[
float2 groomLabLineUv(float2 uv) { return float2(uv.x - 2.0f * floor(0.5f * uv.x), uv.y); }
]]></source>
    </implementation>
    </implementation>
</fragment>)x";

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

// Every occurrence of `from` in `text` replaced; false when there was none.
bool replaceAll(std::string& text, const std::string& from, const std::string& to) {
    bool any = false;
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
        any = true;
    }
    return any;
}

// The lit strands' specular fragment: Maya's hair tube specular (mayaHairTubeShaderSpecular)
// without its bitangent stream. Maya's reads the stream only as the tube's direction when
// tubeDirection is 1; GroomLab draws every tube along the strand (tubeDirection 0), so the
// strands carry no bitangent (the viewport writes a third less per point: display.h, StrandLines)
// and the fragment takes the tangent there too. Patched from this Maya's own XML and registered
// once as groomLabHairTubeSpecular; false (said once) when the XML is not as expected.
bool strandSpecularFragmentRegistered(MHWRender::MFragmentManager& fragments) {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    if (state != 0) return state > 0;
    state = -1;
    if (fragments.hasFragment("groomLabHairTubeSpecular")) return (state = 1) > 0;
    MString specularXml;
    if (!fragments.getFragmentXML("mayaHairTubeShaderSpecular", specularXml)) return false;
    std::string fragment = specularXml.asChar();
    const bool ok = replaceAll(fragment, "mayaHairTubeShaderSpecular", "groomLabHairTubeSpecular") &&
                    replaceOnce(fragment, "<float3  name=\"mayaBitangentIn\" semantic=\"bitangent\" flags=\"varyingInputParam\" />", "") &&
                    (replaceAll(fragment, "float3 Bw, ", "") | replaceAll(fragment, "vec3 Bw, ", "")) &&
                    (replaceAll(fragment, "float4(Bw, 0.0f)", "float4(Tw, 0.0f)") | replaceAll(fragment, "vec4(Bw, 0.0f)", "vec4(Tw, 0.0f)")) &&
                    fragment.find("Bw") == std::string::npos && fragment.find("bitangent") == std::string::npos;
    if (!ok) {
        MGlobal::displayWarning("GroomLab: this Maya's hair specular fragment could not be adapted; the lit strands draw plain");
        return false;
    }
    if (fragments.addShadeFragmentFromBuffer(fragment.c_str(), false).length() == 0) {
        MGlobal::displayWarning("GroomLab: the strands' specular fragment did not register; the lit strands draw plain");
        return false;
    }
    state = 1;
    return true;
}

// Registers `graphName` once: Maya's hair tube shading graph with its color fragment swapped for
// `fragmentXml` (named `fragment`, taking the color fragment's inputs first, then its own; null:
// Maya's own color fragment kept), and, given `geomFragmentXml` (named `geomFragment`), its
// geometry terms (mayaShaderGeom) for that one, the graph's properties given `properties` and
// `values` beside its own. `strands`: the lit strands' graph, its specular fragment swapped for
// the one without a bitangent stream (strandSpecularFragmentRegistered); `cards`: the card graph,
// with kCardCoverageFragment's alpha to coverage after its last fragment. False (said once, with `fallback`: what
// draws instead) when this Maya's hair graph can't be adapted.
bool registerHairGraph(int& state, const char* graphName, const char* fragment, const char* fragmentXml,
                       const std::string& properties, const std::string& values, const char* what, const char* fallback,
                       const char* geomFragment = nullptr, const char* geomFragmentXml = nullptr, bool strands = false,
                       bool cards = false) {
    if (state != 0) return state > 0;
    state = -1;
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MFragmentManager* fragments = renderer ? renderer->getFragmentManager() : nullptr;
    if (!fragments) return false;
    if (fragments->hasFragment(graphName)) return (state = 1) > 0;
    if (strands && !strandSpecularFragmentRegistered(*fragments)) return false;
    MString hairXml;
    if (!fragments->getFragmentXML("mayaHairTubeShaderSurface", hairXml)) return false;
    std::string graph = hairXml.asChar();
    const std::string name(graphName);
    bool ok = replaceOnce(graph, "name=\"mayaHairTubeShaderSurface\" ref=\"mayaHairTubeShaderSurface\"",
                          "name=\"" + name + "\" ref=\"" + name + "\"") &&
              replaceOnce(graph, "</properties>", properties + "    </properties>") &&
              (values.empty() || replaceOnce(graph, "</values>", values + "    </values>")) &&
              (!geomFragment ||
               (replaceOnce(graph, "<fragment_ref name=\"mayaShaderGeom\" ref=\"mayaShaderGeom\" />",
                            std::string("<fragment_ref name=\"mayaShaderGeom\" ref=\"") + geomFragment + "\" />") &&
                replaceAll(graph, "from=\"mayaShaderGeom.mayaShaderGeom\"", std::string("from=\"mayaShaderGeom.") + geomFragment + "\"")));
    if (ok && fragment) {
        const std::string output = std::string("mayaHairTubeShaderColor.") + fragment;
        ok = replaceOnce(graph, "<fragment_ref name=\"mayaHairTubeShaderColor\" ref=\"mayaHairTubeShaderColor\" />",
                         std::string("<fragment_ref name=\"mayaHairTubeShaderColor\" ref=\"") + fragment + "\" />") &&
             replaceOnce(graph, "from=\"mayaHairTubeShaderColor.mayaHairTubeShaderColor\" to=\"mayaADSRCombiner.color\"",
                         "from=\"" + output + "\" to=\"mayaADSRCombiner.color\"") &&
             replaceOnce(graph, "from=\"mayaHairTubeShaderColor.mayaHairTubeShaderColor\" to=\"mayaHairTubeShaderSpecular.color\"",
                         "from=\"" + output + "\" to=\"mayaHairTubeShaderSpecular.color\"") &&
             // The view and the normal (the graph's Vw and Nw, which Viewport 2.0 makes per pixel) to
             // the color fragment too, each through a pass-through feeding both: the iridescence turns
             // with the angle between them. (A property of a stream's semantic of its own, a second
             // tangent say, would have Viewport 2.0 hold a copy of the stream a partial write misses.)
             replaceOnce(graph, "<float3  name=\"Vw\" ref=\"mayaShaderGeom.Vw\" flags=\"varyingInputParam\" />",
                         "<float3  name=\"Vw\" ref=\"groomLabViewPassThrough.input\" flags=\"varyingInputParam\" />") &&
             replaceOnce(graph, "<float3  name=\"Nw\" ref=\"mayaShaderGeom.Nw\" flags=\"varyingInputParam\" />",
                         "<float3  name=\"Nw\" ref=\"groomLabNormalPassThrough.input\" flags=\"varyingInputParam\" />") &&
             replaceOnce(graph, "<fragment_ref name=\"mayaShaderGeom\"",
                         "<fragment_ref name=\"groomLabViewPassThrough\" ref=\"mayafloat3PassThrough\" />\n"
                         "            <fragment_ref name=\"groomLabNormalPassThrough\" ref=\"mayafloat3PassThrough\" />\n"
                         "            <fragment_ref name=\"mayaShaderGeom\"") &&
             replaceOnce(graph, "</connections>",
                         "    <connect from=\"groomLabViewPassThrough.mayafloat3PassThrough\" to=\"mayaShaderGeom.Vw\" name=\"Vw\" />\n"
                         "        <connect from=\"groomLabViewPassThrough.mayafloat3PassThrough\" to=\"mayaHairTubeShaderColor.irView\" "
                         "name=\"irView\" />\n"
                         "        <connect from=\"groomLabNormalPassThrough.mayafloat3PassThrough\" to=\"mayaShaderGeom.Nw\" name=\"Nw\" />\n"
                         "        <connect from=\"groomLabNormalPassThrough.mayafloat3PassThrough\" to=\"mayaHairTubeShaderColor.irNormal\" "
                         "name=\"irNormal\" />\n    </connections>");
    }
    if (ok && strands) {
        // The specular fragment without the bitangent stream, and the graph's own property of it gone.
        ok = replaceOnce(graph, "<fragment_ref name=\"mayaHairTubeShaderSpecular\" ref=\"mayaHairTubeShaderSpecular\" />",
                         "<fragment_ref name=\"mayaHairTubeShaderSpecular\" ref=\"groomLabHairTubeSpecular\" />") &&
             replaceAll(graph, "from=\"mayaHairTubeShaderSpecular.mayaHairTubeShaderSpecular\"",
                        "from=\"mayaHairTubeShaderSpecular.groomLabHairTubeSpecular\"") &&
             replaceOnce(graph,
                         "<float3  name=\"mayaBitangentIn\" ref=\"mayaHairTubeShaderSpecular.mayaBitangentIn\" semantic=\"bitangent\" "
                         "flags=\"varyingInputParam\" />",
                         "") &&
             graph.find("semantic=\"bitangent\"") == std::string::npos;
        // Width as coverage (kStrandCoverageFragment) after the graph's last fragment, reading the
        // uvs (their width, the strand's number) and the world position through pass-throughs
        // feeding the fragments that read them already (the color fragment u alone:
        // kLineUvFragment), its uniforms the graph's own.
        ok = ok &&
             replaceOnce(graph,
                         "<connect from=\"mayaComputeSurfaceFinal.mayaComputeSurfaceFinal\" "
                         "to=\"mayaSurfaceShaderOutput.mayaSurfaceShaderOutput\" name=\"mayaSurfaceShaderOutput\" />",
                         "<connect from=\"mayaComputeSurfaceFinal.mayaComputeSurfaceFinal\" to=\"groomLabStrandCoverage.input\" "
                         "name=\"input\" />\n"
                         "        <connect from=\"groomLabStrandCoverage.groomLabStrandCoverage\" "
                         "to=\"mayaSurfaceShaderOutput.mayaSurfaceShaderOutput\" name=\"mayaSurfaceShaderOutput\" />") &&
             replaceOnce(graph, "<float2  name=\"uvCoord\" ref=\"mayaHairTubeShaderColor.uvCoord\" semantic=\"uvCoord\" "
                                "flags=\"varyingInputParam\" />",
                         "<float2  name=\"uvCoord\" ref=\"groomLabUvPassThrough.uvCoord\" semantic=\"uvCoord\" flags=\"varyingInputParam\" />") &&
             replaceOnce(graph, "<float3  name=\"Pw\" ref=\"maya_FogDepthSurface.Pw\" semantic=\"Pw\" />",
                         "<float3  name=\"Pw\" ref=\"groomLabPwPassThrough.input\" semantic=\"Pw\" />") &&
             replaceOnce(graph, "<fragment_ref name=\"mayaSurfaceShaderOutput\"",
                         "<fragment_ref name=\"groomLabStrandCoverage\" ref=\"groomLabStrandCoverage\" />\n"
                         "            <fragment_ref name=\"groomLabUvPassThrough\" ref=\"groomLabUvPassThrough\" />\n"
                         "            <fragment_ref name=\"groomLabLineUv\" ref=\"groomLabLineUv\" />\n"
                         "            <fragment_ref name=\"groomLabPwPassThrough\" ref=\"mayafloat3PassThrough\" />\n"
                         "            <fragment_ref name=\"mayaSurfaceShaderOutput\"") &&
             replaceOnce(graph, "</connections>",
                         "    <connect from=\"groomLabUvPassThrough.groomLabUvPassThrough\" to=\"groomLabLineUv.uv\" name=\"uv\" />\n"
                         "        <connect from=\"groomLabLineUv.groomLabLineUv\" to=\"mayaHairTubeShaderColor.uvCoord\" "
                         "name=\"uvCoord\" />\n"
                         "        <connect from=\"groomLabUvPassThrough.groomLabUvPassThrough\" to=\"groomLabStrandCoverage.uv\" name=\"uv\" />\n"
                         "        <connect from=\"groomLabPwPassThrough.mayafloat3PassThrough\" to=\"maya_FogDepthSurface.Pw\" name=\"Pw\" />\n"
                         "        <connect from=\"groomLabPwPassThrough.mayafloat3PassThrough\" to=\"groomLabStrandCoverage.Pw\" name=\"Pw\" />\n"
                         "    </connections>") &&
             replaceOnce(graph, "</properties>",
                         "    <float4x4 name=\"lineViewProj\" ref=\"groomLabStrandCoverage.viewProj\" semantic=\"viewprojection\" />\n"
                         "        <float4x4 name=\"lineProjection\" ref=\"groomLabStrandCoverage.projection\" semantic=\"projection\" />\n"
                         "        <float2 name=\"lineViewport\" ref=\"groomLabStrandCoverage.viewport\" semantic=\"viewportPixelSize\" />\n"
                         "        <float name=\"lineCoverage\" ref=\"groomLabStrandCoverage.lineCoverage\" />\n"
                         "        <float name=\"lineWidthScale\" ref=\"groomLabStrandCoverage.lineWidthScale\" />\n"
                         "        <float name=\"lineCoverageFloor\" ref=\"groomLabStrandCoverage.lineCoverageFloor\" />\n"
                         "        <float name=\"lineDensity\" ref=\"groomLabStrandCoverage.lineDensity\" />\n"
                         "        <float name=\"lineLod\" ref=\"groomLabStrandCoverage.lineLod\" />\n"
                         "        <float name=\"lineBlend\" ref=\"groomLabStrandCoverage.lineBlend\" />\n"
                         "        <float name=\"lineSamples\" ref=\"groomLabStrandCoverage.lineSamples\" />\n"
                         "    </properties>") &&
             replaceOnce(graph, "</values>",
                         "<float name=\"lineCoverage\" value=\"0.0\" />\n<float name=\"lineWidthScale\" value=\"1.0\" />\n"
                         "<float name=\"lineCoverageFloor\" value=\"0.0\" />\n<float name=\"lineDensity\" value=\"1.0\" />\n"
                         "<float name=\"lineLod\" value=\"0.0\" />\n<float name=\"lineBlend\" value=\"0.0\" />\n"
                         "<float name=\"lineSamples\" value=\"1.0\" />\n"
                         "    </values>");
    }
    if (ok && cards) {
        // Alpha to coverage (kCardCoverageFragment) after the graph's last fragment, reading the uvs
        // through a pass-through feeding the color fragment too, its map (the color map, bound to it
        // as well), cutoff and size the graph's own (coverageCutoff, coverageMapSize: the color
        // fragment's alpha test has cutoff and mapSize).
        ok = replaceOnce(graph,
                         "<connect from=\"mayaComputeSurfaceFinal.mayaComputeSurfaceFinal\" "
                         "to=\"mayaSurfaceShaderOutput.mayaSurfaceShaderOutput\" name=\"mayaSurfaceShaderOutput\" />",
                         "<connect from=\"mayaComputeSurfaceFinal.mayaComputeSurfaceFinal\" to=\"groomLabCardCoverage.input\" "
                         "name=\"input\" />\n"
                         "        <connect from=\"groomLabCardCoverage.groomLabCardCoverage\" "
                         "to=\"mayaSurfaceShaderOutput.mayaSurfaceShaderOutput\" name=\"mayaSurfaceShaderOutput\" />") &&
             replaceOnce(graph, "<float2  name=\"uvCoord\" ref=\"mayaHairTubeShaderColor.uvCoord\" semantic=\"uvCoord\" "
                                "flags=\"varyingInputParam\" />",
                         "<float2  name=\"uvCoord\" ref=\"groomLabUvPassThrough.uvCoord\" semantic=\"uvCoord\" flags=\"varyingInputParam\" />") &&
             replaceOnce(graph, "<fragment_ref name=\"mayaSurfaceShaderOutput\"",
                         "<fragment_ref name=\"groomLabCardCoverage\" ref=\"groomLabCardCoverage\" />\n"
                         "            <fragment_ref name=\"groomLabUvPassThrough\" ref=\"groomLabUvPassThrough\" />\n"
                         "            <fragment_ref name=\"mayaSurfaceShaderOutput\"") &&
             replaceOnce(graph, "</connections>",
                         "    <connect from=\"groomLabUvPassThrough.groomLabUvPassThrough\" to=\"mayaHairTubeShaderColor.uvCoord\" "
                         "name=\"uvCoord\" />\n"
                         "        <connect from=\"groomLabUvPassThrough.groomLabUvPassThrough\" to=\"groomLabCardCoverage.uv\" name=\"uv\" />\n"
                         "    </connections>") &&
             replaceOnce(graph, "</properties>",
                         "    <texture2 name=\"coverageMap\" ref=\"groomLabCardCoverage.coverageMap\" />\n"
                         "        <sampler name=\"coverageMapSampler\" ref=\"groomLabCardCoverage.coverageMapSampler\" />\n"
                         "        <float name=\"coverageCutoff\" ref=\"groomLabCardCoverage.cutoff\" />\n"
                         "        <float name=\"coverageMapSize\" ref=\"groomLabCardCoverage.mapSize\" />\n"
                         "        <float name=\"coverageSolid\" ref=\"groomLabCardCoverage.coverageSolid\" />\n"
                         "        <float name=\"alphaToCoverage\" ref=\"groomLabCardCoverage.alphaToCoverage\" />\n"
                         "    </properties>") &&
             replaceOnce(graph, "</values>",
                         "<float name=\"coverageCutoff\" value=\"0.5\" />\n<float name=\"coverageMapSize\" value=\"2048.0\" />\n"
                         "<float name=\"coverageSolid\" value=\"1.0\" />\n<float name=\"alphaToCoverage\" value=\"0.0\" />\n"
                         "    </values>");
    }
    for (const auto& [cardFragment, xml] : {std::pair<const char*, const char*>{"groomLabCardCoverage", kCardCoverageFragment},
                                            std::pair<const char*, const char*>{"groomLabUvPassThrough", kUvPassThroughFragment}}) {
        if (cards && ok && !fragments->hasFragment(cardFragment) && fragments->addShadeFragmentFromBuffer(xml, false).length() == 0) {
            MGlobal::displayWarning(MString("GroomLab: the cards' coverage fragment did not register; ") + fallback);
            return false;
        }
    }
    for (const auto& [name, xml] : {std::pair<const char*, const char*>{"groomLabStrandCoverage", strandCoverageFragment().c_str()},
                                    std::pair<const char*, const char*>{"groomLabUvPassThrough", kUvPassThroughFragment},
                                    std::pair<const char*, const char*>{"groomLabLineUv", kLineUvFragment}}) {
        if (strands && ok && !fragments->hasFragment(name) && fragments->addShadeFragmentFromBuffer(xml, false).length() == 0) {
            MGlobal::displayWarning(MString("GroomLab: the strands' coverage fragments did not register; ") + fallback);
            return false;
        }
    }
    if (!ok) {
        MGlobal::displayWarning(MString("GroomLab: this Maya's hair shading graph could not be adapted for ") + what + "; " + fallback);
        return false;
    }
    if (geomFragmentXml && !fragments->hasFragment(geomFragment) &&
        fragments->addShadeFragmentFromBuffer(geomFragmentXml, false).length() == 0) {
        MGlobal::displayWarning(MString("GroomLab: the shading fragment for ") + what + " did not register; " + fallback);
        return false;
    }
    // (A color fragment two graphs share is registered by the first; the plain graph has none.)
    if (fragment && !fragments->hasFragment(fragment) && fragments->addShadeFragmentFromBuffer(fragmentXml, false).length() == 0) {
        MGlobal::displayWarning(MString("GroomLab: the shading fragment for ") + what + " did not register; " + fallback);
        return false;
    }
    if (fragments->addFragmentGraphFromBuffer(graph.c_str()).length() == 0) {
        MGlobal::displayWarning(MString("GroomLab: the shading graph for ") + what + " did not register; " + fallback);
        return false;
    }
    state = 1;
    return true;
}

}  // namespace

// Registers the card surface graph once (Maya's hair graph, patched); false when this Maya's
// hair graph can't be adapted, said once: the cards then draw as a plain surface.
bool cardShaderRegistered() {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    return registerHairGraph(state, "groomLabCardSurface", "groomLabCardColor", kCardColorFragment,
                             "    <float name=\"cutoff\" ref=\"mayaHairTubeShaderColor.cutoff\" />\n"
                             "        <float name=\"solid\" ref=\"mayaHairTubeShaderColor.solid\" />\n"
                             "        <float3 name=\"solidColor\" ref=\"mayaHairTubeShaderColor.solidColor\" />\n"
                             "        <float name=\"mapSize\" ref=\"mayaHairTubeShaderColor.mapSize\" />\n"
                             "        <float name=\"alphaTest\" ref=\"mayaHairTubeShaderColor.alphaTest\" />\n"
                             "        <float4 name=\"tint\" ref=\"mayaHairTubeShaderColor.tint\" semantic=\"fcolor\" "
                             "flags=\"varyingInputParam\" />\n"
                             "        <float3 name=\"cardNormal\" ref=\"mayaShaderGeom.cardNormal\" semantic=\"bitangent\" "
                             "flags=\"varyingInputParam\" />\n"
                             "        <float3 name=\"iridescenceColor\" ref=\"mayaHairTubeShaderColor.iridescenceColor\" />\n"
                             "        <float name=\"iridescenceShift\" ref=\"mayaHairTubeShaderColor.iridescenceShift\" />\n"
                             "        <texture2 name=\"irMap\" ref=\"mayaHairTubeShaderColor.irMap\" />\n"
                             "        <sampler name=\"irMapSampler\" ref=\"mayaHairTubeShaderColor.irMapSampler\" />\n"
                             "        <float name=\"cardColorLook\" ref=\"mayaHairTubeShaderColor.cardColorLook\" />\n"
                             "        <float3 name=\"cardFromRec709R\" ref=\"mayaHairTubeShaderColor.cardFromRec709R\" />\n"
                             "        <float3 name=\"cardFromRec709G\" ref=\"mayaHairTubeShaderColor.cardFromRec709G\" />\n"
                             "        <float3 name=\"cardFromRec709B\" ref=\"mayaHairTubeShaderColor.cardFromRec709B\" />\n",
                             "<float name=\"cutoff\" value=\"0.5\" />\n<float name=\"solid\" value=\"1.0\" />\n"
                             "<float3 name=\"solidColor\" value=\"0.58,0.50,0.42\" />\n<float name=\"mapSize\" value=\"2048.0\" />\n"
                             "<float name=\"alphaTest\" value=\"1.0\" />\n<float name=\"cardColorLook\" value=\"1.0\" />\n"
                             "<float3 name=\"cardFromRec709R\" value=\"1.0,0.0,0.0\" />\n"
                             "<float3 name=\"cardFromRec709G\" value=\"0.0,1.0,0.0\" />\n"
                             "<float3 name=\"cardFromRec709B\" value=\"0.0,0.0,1.0\" />\n",
                             "the cards", "they draw as a plain surface", "groomLabCardShaderGeom", kCardGeomFragment, false, true);
}

// The lit strands' graph in their own colors (kHairColorFragment), registered once; false when
// it can't be: the lit strands then draw in one color (the strands' mean), as before.
bool hairColorShaderRegistered() {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    return registerHairGraph(state, "groomLabHairSurface", "groomLabHairColor", kHairColorFragment,
                             "    <float4 name=\"strandColor\" ref=\"mayaHairTubeShaderColor.strandColor\" semantic=\"fcolor\" "
                             "flags=\"varyingInputParam\" />\n"
                             "        <float3 name=\"iridescenceColor\" ref=\"mayaHairTubeShaderColor.iridescenceColor\" />\n"
                             "        <float name=\"iridescenceShift\" ref=\"mayaHairTubeShaderColor.iridescenceShift\" />\n",
                             "", "the strands' own colors", "the lit strands draw in one color", nullptr, nullptr, true);
}

// The lit strands' graph in one color (the item's): Maya's hair tube shading as it is, with the
// specular fragment without the bitangent stream (the strands carry none). Registered once; false
// when it can't be: the lit strands then draw with a plain lit shader.
bool plainHairShaderRegistered() {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    return registerHairGraph(state, "groomLabHairTubeSurface", nullptr, nullptr, "", "", "the lit strands",
                             "they draw with a plain lit shader", nullptr, nullptr, true);
}

// The scales' surface (DisplayGeometry::scaleSurface): the hair graph in the vertices' own colors
// (kHairColorFragment, the strands' HairColor), lit from the scale's normals as the cards are
// (kCardGeomFragment: the normal carried in the bitangent stream), its highlight along the scale.
// Registered once; false when it can't be: the scales then draw as a plain surface.
bool scaleShaderRegistered() {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    return registerHairGraph(state, "groomLabScaleSurface", "groomLabHairColor", kHairColorFragment,
                             "    <float4 name=\"strandColor\" ref=\"mayaHairTubeShaderColor.strandColor\" semantic=\"fcolor\" "
                             "flags=\"varyingInputParam\" />\n"
                             "        <float3 name=\"cardNormal\" ref=\"mayaShaderGeom.cardNormal\" semantic=\"bitangent\" "
                             "flags=\"varyingInputParam\" />\n"
                             "        <float3 name=\"iridescenceColor\" ref=\"mayaHairTubeShaderColor.iridescenceColor\" />\n"
                             "        <float name=\"iridescenceShift\" ref=\"mayaHairTubeShaderColor.iridescenceShift\" />\n",
                             "", "the scales", "they draw as a plain surface", "groomLabCardShaderGeom", kCardGeomFragment);
}
}  // namespace groomlab::maya
