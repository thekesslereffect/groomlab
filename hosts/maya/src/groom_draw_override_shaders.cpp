// SPDX-License-Identifier: Apache-2.0
// The draw override's shaders: the overlay and strand lines' effects, width as coverage's draw
// callbacks, the lit strands' shaders and each item's shader settings (style).

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

#include <maya/MFrameContext.h>
#include <maya/MGlobal.h>
#include <maya/MDrawContext.h>
#include <maya/MRenderTargetManager.h>
#include <maya/MHWGeometry.h>
#include <maya/MPlug.h>
#include <maya/MShaderManager.h>
#include <maya/MStateManager.h>
#include <maya/MTextureManager.h>
#include <maya/MViewport2Renderer.h>

namespace groomlab::maya {

namespace {

// The overlay effect: guides, parts and flow arrows as lines gLineWidth pixels wide, each
// vertex pulled toward the camera by gPull (never past halfway to it) before projection, so
// the lines show over the strands, tubes and cards around them while the head still hides
// what is behind it. Colors per vertex (gUseVertexColor) or the item's. One effect per
// language Viewport 2.0 takes effects in (overlayEffectText): HLSL for DirectX 11, OGSFX (GLSL)
// for the OpenGL Core Profile, CgFX for legacy OpenGL. The first two widen the lines in a
// geometry shader; the CgFX one draws them a pixel wide.
const char* kOverlayEffectHlsl = R"x(// GroomLab overlay lines (DirectX 11)
float4x4 gWXf : World;
float4x4 gVPXf : ViewProjection;
float4x4 gVIXf : ViewInverse;
float2 gViewport : ViewportPixelSize;

float gPull = 0.0;
float gLineWidth = 2.0;
float gUseVertexColor = 0.0;
float4 gColor = float4(1.0, 1.0, 1.0, 1.0);

struct appdata {
    float3 Position : POSITION;
    float4 Color : COLOR0;
};

struct vertexOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;
};

vertexOutput overlayVS(appdata IN) {
    vertexOutput OUT = (vertexOutput)0;
    float3 Pw = mul(float4(IN.Position, 1.0), gWXf).xyz;
    float3 toCamera = gVIXf[3].xyz - Pw;
    float away = length(toCamera);
    float pull = gPull * saturate(IN.Color.a);  // the alpha: how far this vertex may go before something in the scene
    if (pull > 0.0 && away > 1e-6) Pw += toCamera / away * min(pull, 0.5 * away);
    OUT.HPosition = mul(float4(Pw, 1.0), gVPXf);
    OUT.Color = gUseVertexColor > 0.5 ? IN.Color : gColor;
    OUT.Color.a = 1.0;
    return OUT;
}

[maxvertexcount(4)]
void overlayGS(line vertexOutput IN[2], inout TriangleStream<vertexOutput> stream) {
    if (IN[0].HPosition.w <= 0.0 || IN[1].HPosition.w <= 0.0) return;  // behind the camera
    float2 p0 = IN[0].HPosition.xy / IN[0].HPosition.w;
    float2 p1 = IN[1].HPosition.xy / IN[1].HPosition.w;
    float2 d = (p1 - p0) * gViewport * 0.5;  // pixels
    float len = length(d);
    d = len > 1e-6 ? d / len : float2(1.0, 0.0);
    float2 off = float2(-d.y, d.x) * (0.5 * max(gLineWidth, 1.0)) / (gViewport * 0.5);  // half a width, in NDC
    vertexOutput o;
    o = IN[0]; o.HPosition = float4((p0 + off) * IN[0].HPosition.w, IN[0].HPosition.zw); stream.Append(o);
    o = IN[0]; o.HPosition = float4((p0 - off) * IN[0].HPosition.w, IN[0].HPosition.zw); stream.Append(o);
    o = IN[1]; o.HPosition = float4((p1 + off) * IN[1].HPosition.w, IN[1].HPosition.zw); stream.Append(o);
    o = IN[1]; o.HPosition = float4((p1 - off) * IN[1].HPosition.w, IN[1].HPosition.zw); stream.Append(o);
    stream.RestartStrip();
}

float4 overlayPS(vertexOutput IN) : SV_Target {
    return IN.Color;
}

technique10 Main {
    pass p0 {
        SetVertexShader(CompileShader(vs_4_0, overlayVS()));
        SetGeometryShader(CompileShader(gs_4_0, overlayGS()));
        SetPixelShader(CompileShader(ps_4_0, overlayPS()));
    }
}
)x";

// OGSFX: GLSL stages in an effect file's frame. Each stage's inputs and outputs are attribute
// blocks (the stages' blocks match member by member); matrices multiply column vectors, so the
// camera is the view inverse's last column.
const char* kOverlayEffectGlsl = R"x(// GroomLab overlay lines (OpenGL Core Profile: OGSFX)
uniform mat4 gWXf : World;
uniform mat4 gVPXf : ViewProjection;
uniform mat4 gVIXf : ViewInverse;
uniform vec2 gViewport : ViewportPixelSize;

uniform float gPull = 0.0;
uniform float gLineWidth = 2.0;
uniform float gUseVertexColor = 0.0;
uniform vec4 gColor = {1.0, 1.0, 1.0, 1.0};

attribute appdata {
    vec3 inPosition : POSITION;
    vec4 inColor : COLOR0;
};

attribute vertexOutput {
    vec4 Color;
};

attribute geometryInput {
    vec4 Color;
};

attribute pixelInput {
    vec4 Color;
};

attribute pixelOutput {
    vec4 outColor : COLOR0;
};

GLSLShader overlayVS {
    void main() {
        vec3 Pw = (gWXf * vec4(inPosition, 1.0)).xyz;
        vec3 toCamera = gVIXf[3].xyz - Pw;
        float away = length(toCamera);
        float pull = gPull * clamp(inColor.a, 0.0, 1.0);  // the alpha: how far this vertex may go before something in the scene
        if (pull > 0.0 && away > 1e-6) Pw += toCamera / away * min(pull, 0.5 * away);
        gl_Position = gVPXf * vec4(Pw, 1.0);
        vsOut.Color = gUseVertexColor > 0.5 ? inColor : gColor;
        vsOut.Color.a = 1.0;
    }
}

GLSLShader overlayGS {
    layout(lines) in;
    layout(triangle_strip, max_vertices = 4) out;

    void corner(int i, vec2 p) {
        gl_Position = vec4(p * gl_in[i].gl_Position.w, gl_in[i].gl_Position.zw);
        gsOut.Color = gsIn[i].Color;
        EmitVertex();
    }

    void main() {
        vec4 h0 = gl_in[0].gl_Position, h1 = gl_in[1].gl_Position;
        if (h0.w <= 0.0 || h1.w <= 0.0) return;  // behind the camera
        vec2 p0 = h0.xy / h0.w;
        vec2 p1 = h1.xy / h1.w;
        vec2 d = (p1 - p0) * gViewport * 0.5;  // pixels
        float len = length(d);
        d = len > 1e-6 ? d / len : vec2(1.0, 0.0);
        vec2 off = vec2(-d.y, d.x) * (0.5 * max(gLineWidth, 1.0)) / (gViewport * 0.5);  // half a width, in NDC
        corner(0, p0 + off);
        corner(0, p0 - off);
        corner(1, p1 + off);
        corner(1, p1 - off);
        EndPrimitive();
    }
}

GLSLShader overlayPS {
    void main() {
        outColor = psIn.Color;
    }
}

technique Main {
    pass p0 {
        VertexShader (in appdata, out vertexOutput vsOut) = overlayVS;
        GeometryShader (in geometryInput gsIn, out pixelInput gsOut) = overlayGS;
        PixelShader (in pixelInput psIn, out pixelOutput) = overlayPS;
    }
}
)x";

const char* kOverlayEffectCg = R"x(// GroomLab overlay lines (legacy OpenGL: CgFX, GLSL profiles)
float4x4 gWXf : World < bool rowMajor = true; >;
float4x4 gVPXf : ViewProjection < bool rowMajor = true; >;
float4x4 gVIXf : ViewInverse < bool rowMajor = true; >;

float gPull = 0.0;
float gLineWidth = 2.0;
float gUseVertexColor = 0.0;
float4 gColor = float4(1.0, 1.0, 1.0, 1.0);

struct appdata {
    float3 Position : POSITION;
    float4 Color : COLOR0;
};

struct vertexOutput {
    float4 HPosition : POSITION;
    float4 Color : COLOR0;
};

vertexOutput overlayVS(appdata IN) {
    vertexOutput OUT;
    float3 Pw = mul(float4(IN.Position, 1.0), gWXf).xyz;
    float3 toCamera = gVIXf[3].xyz - Pw;
    float away = length(toCamera);
    float pull = gPull * saturate(IN.Color.a);  // the alpha: how far this vertex may go before something in the scene
    if (pull > 0.0 && away > 1e-6) Pw += toCamera / away * min(pull, 0.5 * away);
    OUT.HPosition = mul(float4(Pw, 1.0), gVPXf);
    OUT.Color = gUseVertexColor > 0.5 ? IN.Color : gColor;
    OUT.Color.a = 1.0;
    return OUT;
}

float4 overlayPS(vertexOutput IN) : COLOR0 {
    return IN.Color;
}

technique Main {
    pass p0 {
        VertexShader = compile glslv overlayVS();
        PixelShader = compile glslf overlayPS();
    }
}
)x";

// The overlay effect in the language of the renderer's draw API: the Core Profile takes OGSFX
// alone (CgFX fails there to parse), legacy OpenGL CgFX.
const char* overlayEffectText() {
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    switch (renderer ? renderer->drawAPI() : MHWRender::kNone) {
        case MHWRender::kDirectX11: return kOverlayEffectHlsl;
        case MHWRender::kOpenGLCoreProfile: return kOverlayEffectGlsl;
        case MHWRender::kOpenGL: return kOverlayEffectCg;
        default: return nullptr;
    }
}

}  // namespace

MHWRender::MShaderInstance* acquireOverlayShader(const MHWRender::MShaderManager& shaders) {
    static int state = 0;  // 0 not tried, 1 compiles, -1 failed
    if (state < 0) return nullptr;
    const char* text = overlayEffectText();
    if (!text) return nullptr;  // no device draws: nothing to say
    MHWRender::MShaderInstance* shader =
        shaders.getEffectsBufferShader(text, static_cast<unsigned>(std::strlen(text)), "Main", nullptr, 0, true);
    if (!shader) {
        state = -1;
        MGlobal::displayWarning(MString("GroomLab: the overlay shader did not compile, so guides, parts and flow draw "
                                        "depth-tested with the strands: ") +
                                MHWRender::MShaderManager::getLastError());
        return nullptr;
    }
    state = 1;
    return shader;
}

bool overlayShaderAvailable(const MHWRender::MShaderManager& shaders) {
    static int state = 0;
    if (state == 0) {
        MHWRender::MShaderInstance* probe = acquireOverlayShader(shaders);
        state = probe ? 1 : -1;
        if (probe) shaders.releaseShader(probe);
    }
    return state > 0;
}

namespace {

// The dots effect: the guide dots, a Scatter's roots and a Clump's centers (DisplayGeometry::points),
// each point a round dot gPointSize pixels across facing the camera, made by a geometry shader
// from the point alone, so the GPU draws any number of them and a camera move rebuilds nothing.
// Pulled toward the camera as the overlay lines are (gPull, each point's limit in its alpha).
// DirectX 11 and the OpenGL Core Profile; legacy OpenGL has no geometry stage in CgFX here, so
// there the dots stay the CPU's (addDots).
const char* kDotsEffectHlsl = R"x(// GroomLab dots (DirectX 11)
float4x4 gWXf : World;
float4x4 gVPXf : ViewProjection;
float4x4 gVIXf : ViewInverse;
float2 gViewport : ViewportPixelSize;

float gPull = 0.0;
float gPointSize = 6.0;

struct appdata {
    float3 Position : POSITION;
    float4 Color : COLOR0;
};

struct vertexOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;
};

struct dotOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;
    float2 Corner : TEXCOORD0;
};

vertexOutput dotsVS(appdata IN) {
    vertexOutput OUT = (vertexOutput)0;
    float3 Pw = mul(float4(IN.Position, 1.0), gWXf).xyz;
    float3 toCamera = gVIXf[3].xyz - Pw;
    float away = length(toCamera);
    float pull = gPull * saturate(IN.Color.a);  // the alpha: how far this point may go before something in the scene
    if (pull > 0.0 && away > 1e-6) Pw += toCamera / away * min(pull, 0.5 * away);
    OUT.HPosition = mul(float4(Pw, 1.0), gVPXf);
    OUT.Color = float4(IN.Color.rgb, 1.0);
    return OUT;
}

[maxvertexcount(4)]
void dotsGS(point vertexOutput IN[1], inout TriangleStream<dotOutput> stream) {
    float4 h = IN[0].HPosition;
    if (h.w <= 0.0) return;  // behind the camera
    float2 r = (0.5 * max(gPointSize, 1.0)) / (gViewport * 0.5);  // half the size, in NDC
    dotOutput o;
    o.Color = IN[0].Color;
    o.Corner = float2(-1.0, -1.0); o.HPosition = float4(h.xy + o.Corner * r * h.w, h.zw); stream.Append(o);
    o.Corner = float2(1.0, -1.0); o.HPosition = float4(h.xy + o.Corner * r * h.w, h.zw); stream.Append(o);
    o.Corner = float2(-1.0, 1.0); o.HPosition = float4(h.xy + o.Corner * r * h.w, h.zw); stream.Append(o);
    o.Corner = float2(1.0, 1.0); o.HPosition = float4(h.xy + o.Corner * r * h.w, h.zw); stream.Append(o);
    stream.RestartStrip();
}

float4 dotsPS(dotOutput IN) : SV_Target {
    if (dot(IN.Corner, IN.Corner) > 1.0) discard;  // round
    return IN.Color;
}

technique10 Main {
    pass p0 {
        SetVertexShader(CompileShader(vs_4_0, dotsVS()));
        SetGeometryShader(CompileShader(gs_4_0, dotsGS()));
        SetPixelShader(CompileShader(ps_4_0, dotsPS()));
    }
}
)x";

const char* kDotsEffectGlsl = R"x(// GroomLab dots (OpenGL Core Profile: OGSFX)
uniform mat4 gWXf : World;
uniform mat4 gVPXf : ViewProjection;
uniform mat4 gVIXf : ViewInverse;
uniform vec2 gViewport : ViewportPixelSize;

uniform float gPull = 0.0;
uniform float gPointSize = 6.0;

attribute appdata {
    vec3 inPosition : POSITION;
    vec4 inColor : COLOR0;
};

attribute vertexOutput {
    vec4 Color;
};

attribute geometryInput {
    vec4 Color;
};

attribute pixelInput {
    vec4 Color;
    vec2 Corner;
};

attribute pixelOutput {
    vec4 outColor : COLOR0;
};

GLSLShader dotsVS {
    void main() {
        vec3 Pw = (gWXf * vec4(inPosition, 1.0)).xyz;
        vec3 toCamera = gVIXf[3].xyz - Pw;
        float away = length(toCamera);
        float pull = gPull * clamp(inColor.a, 0.0, 1.0);  // the alpha: how far this point may go before something in the scene
        if (pull > 0.0 && away > 1e-6) Pw += toCamera / away * min(pull, 0.5 * away);
        gl_Position = gVPXf * vec4(Pw, 1.0);
        vsOut.Color = vec4(inColor.rgb, 1.0);
    }
}

GLSLShader dotsGS {
    layout(points) in;
    layout(triangle_strip, max_vertices = 4) out;

    void corner(vec4 h, vec2 r, vec2 c) {
        gl_Position = vec4(h.xy + c * r * h.w, h.zw);
        gsOut.Color = gsIn[0].Color;
        gsOut.Corner = c;
        EmitVertex();
    }

    void main() {
        vec4 h = gl_in[0].gl_Position;
        if (h.w <= 0.0) return;  // behind the camera
        vec2 r = (0.5 * max(gPointSize, 1.0)) / (gViewport * 0.5);  // half the size, in NDC
        corner(h, r, vec2(-1.0, -1.0));
        corner(h, r, vec2(1.0, -1.0));
        corner(h, r, vec2(-1.0, 1.0));
        corner(h, r, vec2(1.0, 1.0));
        EndPrimitive();
    }
}

GLSLShader dotsPS {
    void main() {
        if (dot(psIn.Corner, psIn.Corner) > 1.0) discard;  // round
        outColor = psIn.Color;
    }
}

technique Main {
    pass p0 {
        VertexShader (in appdata, out vertexOutput vsOut) = dotsVS;
        GeometryShader (in geometryInput gsIn, out pixelInput gsOut) = dotsGS;
        PixelShader (in pixelInput psIn, out pixelOutput) = dotsPS;
    }
}
)x";

const char* dotsEffectText() {
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    switch (renderer ? renderer->drawAPI() : MHWRender::kNone) {
        case MHWRender::kDirectX11: return kDotsEffectHlsl;
        case MHWRender::kOpenGLCoreProfile: return kDotsEffectGlsl;
        default: return nullptr;  // legacy OpenGL: the CPU's dots
    }
}

}  // namespace

MHWRender::MShaderInstance* acquireDotsShader(const MHWRender::MShaderManager& shaders) {
    static int state = 0;  // 0 not tried, 1 compiles, -1 failed
    if (state < 0) return nullptr;
    const char* text = dotsEffectText();
    if (!text) return nullptr;
    MHWRender::MShaderInstance* shader =
        shaders.getEffectsBufferShader(text, static_cast<unsigned>(std::strlen(text)), "Main", nullptr, 0, true);
    if (!shader) {
        state = -1;
        MGlobal::displayWarning(MString("GroomLab: the dots shader did not compile, so the dots draw on the CPU: ") +
                                MHWRender::MShaderManager::getLastError());
        return nullptr;
    }
    state = 1;
    return shader;
}

bool dotsShaderAvailable(const MHWRender::MShaderManager& shaders) {
    static int state = 0;
    if (state == 0) {
        if (!dotsEffectText()) return false;  // (no device yet, or legacy OpenGL: asked again)
        MHWRender::MShaderInstance* probe = acquireDotsShader(shaders);
        state = probe ? 1 : -1;
        if (probe) shaders.releaseShader(probe);
    }
    return state > 0;
}

namespace {

// ---- Width as coverage (displayWidthCoverage; display.h lineCoverage) ---------------------------
//
// The strand lines draw a pixel wide; with width as coverage each point covers the share of that
// pixel its width covers: the width the uvs stream carries second (u with the strand's number,
// width: StrandLines::writeUvs), times ribbonWidthScale, projected to pixels (half the viewport's
// height times the projection's [1][1], over the clip w: the view depth, or 1 orthographic),
// clamped to [lineCoverageFloor, 1], then taken to the render's density (lineDensity: the
// preview's, display.h lineCoverageAtDensity; 1 with displayCoverageDensity off); a negative
// width (strands without widths) draws opaque.
//
// The coverage is drawn as hashed alpha testing (display.h lineHashThreshold, lineSampleMask; the
// shaders' code engine/gpu/shaders/line_coverage.glsl, where @LINE_COVERAGE@ stands):
// each fragment is kept, opaque, where its coverage reaches a threshold hashed from its strand
// (the uvs' number) and the pixel-long stretch of the strand it lies on, so the strands crossing a
// pixel each cover it on their own (n of them 1 - (1 - c)^n of it, as ribbons) and the pattern
// stays on the strands as the camera moves. On a multisampled target (the viewport's multisample
// anti-aliasing) the flat lines' fragment writes a coverage mask instead (SV_Coverage,
// gl_SampleMask): coverage x samples of the pixel's samples on average, a run of them turned by
// the strand's hash, so strands crossing a pixel fill different samples. With one sample a pixel
// the top of the range blends instead (display.h lineKeepShare: kept with that share, drawn with
// alpha coverage / share), so a strand seen close draws smooth rather than stippled. The items
// stay in the opaque pass with depth written (the head and the guides hide what is behind them,
// the hair keeps its shadows and ambient occlusion), nothing sorted, their draws taking an alpha
// blending state of their own (coveragePreDraw).
//
// The flat lines (one color, or the color modes' per-vertex colors) draw with the effect below,
// the coverage made in its vertex stage. The lit lines keep Maya's hair tube shading graph, which
// takes kStrandCoverageFragment after its last fragment (registerHairGraph): there the coverage is
// made per pixel from the world position, the same arithmetic, and the fragment kept or not as a
// whole (a fragment of the graph cannot write the mask).
const char* kLinesEffectHlsl = R"x(// GroomLab strand lines, width as coverage (DirectX 11)
float4x4 gWVPXf : WorldViewProjection;
float4x4 gProjXf : Projection;
float2 gViewport : ViewportPixelSize;

float4 solidColor = float4(1.0, 1.0, 1.0, 1.0);
float lineWidthScale = 1.0;
float lineCoverageFloor = 0.0;
float lineDensity = 1.0;
float lineLod = 0.0;
float lineBlend = 0.0;  // of two passes: 1 the opaque fragments alone, 2 the blended (0: both)

struct appdata {
    float3 Position : POSITION;
    float2 Uv : TEXCOORD0;
};

struct appdataColored {
    float3 Position : POSITION;
    float4 Color : COLOR0;
    float2 Uv : TEXCOORD0;
};

struct vertexOutput {
    float4 HPosition : SV_Position;
    float4 Color : COLOR0;  // rgb; a: the coverage
    float U : TEXCOORD0;    // u plus twice the strand's number
    float Own : TEXCOORD1;  // the coverage before the density's
};

struct pixelOutput {
    float4 Color : SV_Target;
    uint Mask : SV_Coverage;
};

@LINE_COVERAGE@
// line_coverage.glsl lineCoverageOf on the effect's uniforms.
float2 coverage(float width, float w) {
    return lineCoverageOf(width, w, lineWidthScale, 0.5 * gProjXf[1][1] * gViewport.y, lineCoverageFloor, lineDensity, lineLod);
}

vertexOutput plainVS(appdata IN) {
    vertexOutput OUT;
    OUT.HPosition = mul(float4(IN.Position, 1.0), gWVPXf);
    float2 c = coverage(IN.Uv.y, OUT.HPosition.w);
    OUT.Color = float4(solidColor.rgb, c.x);
    OUT.U = IN.Uv.x;
    OUT.Own = c.y;
    return OUT;
}

vertexOutput coloredVS(appdataColored IN) {
    vertexOutput OUT;
    OUT.HPosition = mul(float4(IN.Position, 1.0), gWVPXf);
    float2 c = coverage(IN.Uv.y, OUT.HPosition.w);
    OUT.Color = float4(IN.Color.rgb, c.x);
    OUT.U = IN.Uv.x;
    OUT.Own = c.y;
    return OUT;
}

pixelOutput linesPS(vertexOutput IN) {
    float perPixel = length(float2(ddx(IN.U), ddy(IN.U)));
    float number = floor(0.5 * IN.U);
    uint strand = uint(max(number, 0.0));
    uint samples = GetRenderTargetSampleCount();
    float t = lineThreshold(strand, IN.U - 2.0 * number, perPixel, samples);
    float keep = samples > 1u ? IN.Color.a : lineKeepShare(IN.Color.a, IN.Own);
    uint mask = lineSampleMask(keep, t, strand, samples);
    if (mask == 0u) discard;
    bool opaque = IN.Color.a >= 0.9999 * keep;
    if (lineBlend > 1.5 ? opaque : (lineBlend > 0.5 && !opaque)) discard;
    pixelOutput OUT;
    OUT.Color = float4(IN.Color.rgb, IN.Color.a / max(keep, 1e-6));
    OUT.Mask = mask;
    return OUT;
}

technique11 Plain {
    pass p0 {
        SetVertexShader(CompileShader(vs_5_0, plainVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, linesPS()));
    }
}

technique11 Colored {
    pass p0 {
        SetVertexShader(CompileShader(vs_5_0, coloredVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, linesPS()));
    }
}
)x";

const char* kLinesEffectGlsl = R"x(#version 430
// GroomLab strand lines, width as coverage (OpenGL Core Profile: OGSFX)
uniform mat4 gWVPXf : WorldViewProjection;
uniform mat4 gProjXf : Projection;
uniform vec2 gViewport : ViewportPixelSize;

uniform vec4 solidColor = {1.0, 1.0, 1.0, 1.0};
uniform float lineWidthScale = 1.0;
uniform float lineCoverageFloor = 0.0;
uniform float lineDensity = 1.0;
uniform float lineLod = 0.0;
uniform float lineBlend = 0.0;  // of two passes: 1 the opaque fragments alone, 2 the blended (0: both)

attribute appdata {
    vec3 inPosition : POSITION;
    vec2 inUv : TEXCOORD0;
};

attribute appdataColored {
    vec3 inPosition : POSITION;
    vec4 inColor : COLOR0;
    vec2 inUv : TEXCOORD0;
};

attribute vertexOutput {
    vec4 Color;  // rgb; a: the coverage
    float U;     // u plus twice the strand's number
    float Own;   // the coverage before the density's
};

attribute pixelOutput {
    vec4 outColor : COLOR0;
};

GLSLShader plainVS {
@LINE_COVERAGE@
    void main() {
        gl_Position = gWVPXf * vec4(inPosition, 1.0);
        vec2 c = lineCoverageOf(inUv.y, gl_Position.w, lineWidthScale, 0.5 * gProjXf[1][1] * gViewport.y, lineCoverageFloor,
                                lineDensity, lineLod);
        vsOut.Own = c.y;  // (a thinned class's strand: 0, hashed alone)
        vsOut.Color = vec4(solidColor.rgb, c.x);
        vsOut.U = inUv.x;
    }
}

GLSLShader coloredVS {
@LINE_COVERAGE@
    void main() {
        gl_Position = gWVPXf * vec4(inPosition, 1.0);
        vec2 c = lineCoverageOf(inUv.y, gl_Position.w, lineWidthScale, 0.5 * gProjXf[1][1] * gViewport.y, lineCoverageFloor,
                                lineDensity, lineLod);
        vsOut.Own = c.y;  // (a thinned class's strand: 0, hashed alone)
        vsOut.Color = vec4(inColor.rgb, c.x);
        vsOut.U = inUv.x;
    }
}

GLSLShader linesPS {
@LINE_COVERAGE@
    void main() {
        float perPixel = length(vec2(dFdx(psIn.U), dFdy(psIn.U)));
        float number = floor(0.5 * psIn.U);
        uint strand = uint(max(number, 0.0));
        uint samples = uint(max(gl_NumSamples, 1));
        float t = lineThreshold(strand, psIn.U - 2.0 * number, perPixel, samples);
        float keep = samples > 1u ? psIn.Color.a : lineKeepShare(psIn.Color.a, psIn.Own);
        uint mask = lineSampleMask(keep, t, strand, samples);
        if (mask == 0u) discard;
        bool opaque = psIn.Color.a >= 0.9999 * keep;
        if (lineBlend > 1.5 ? opaque : (lineBlend > 0.5 && !opaque)) discard;
        gl_SampleMask[0] = int(mask);
        outColor = vec4(psIn.Color.rgb, psIn.Color.a / max(keep, 1e-6));
    }
}

technique Plain {
    pass p0 {
        VertexShader (in appdata, out vertexOutput vsOut) = plainVS;
        PixelShader (in vertexOutput psIn, out pixelOutput) = linesPS;
    }
}

technique Colored {
    pass p0 {
        VertexShader (in appdataColored, out vertexOutput vsOut) = coloredVS;
        PixelShader (in vertexOutput psIn, out pixelOutput) = linesPS;
    }
}
)x";

// An effect's text with line_coverage.glsl in its words where @LINE_COVERAGE@ stands.
std::string withLineCoverage(const char* effect, bool hlsl) {
    std::string text = effect;
    const std::string token = "@LINE_COVERAGE@";
    const std::string& shared = lineCoverageText(hlsl);
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + shared.size())) {
        text.replace(at, token.size(), shared);
    }
    return text;
}

// The flat lines' effect in the renderer's draw API's language (null: none, legacy OpenGL: the
// stock shaders draw them opaque).
const char* linesEffectText() {
    static const std::string hlsl = withLineCoverage(kLinesEffectHlsl, true);
    static const std::string glsl = glslForDevice(withLineCoverage(kLinesEffectGlsl, false));
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    switch (renderer ? renderer->drawAPI() : MHWRender::kNone) {
        case MHWRender::kDirectX11: return hlsl.c_str();
        case MHWRender::kOpenGLCoreProfile: return glsl.c_str();
        default: return nullptr;
    }
}

}  // namespace

// Whether a draw is in a pass drawing the scene's colors (not shadows, depth, selection...): the
// color pass, or one of the shaded beauty graph's, where Viewport 2.0 draws transparent items
// (transparentGeometry, and its cutOffAlpha pass before them; the level of detail's blend item is
// one): without the lights and the blending there, the strands it draws come out unlit, black.
bool colorPass(const MHWRender::MDrawContext& context) {
    const MStringArray& semantics = context.getPassContext().passSemantics();
    bool color = false;
    for (unsigned i = 0; i < semantics.length(); ++i) {
        const MString& s = semantics[i];
        if (s == MHWRender::MPassContext::kShadowPassSemantic || s == MHWRender::MPassContext::kPointLightShadowPassSemantic ||
            s == MHWRender::MPassContext::kDepthPassSemantic || s == MHWRender::MPassContext::kNormalDepthPassSemantic ||
            s == MHWRender::MPassContext::kSelectionPassSemantic || s == MHWRender::MPassContext::kMotionVectorPassSemantic) {
            return false;
        }
        if (s == MHWRender::MPassContext::kColorPassSemantic || s == "shadedBeautyGraphSemantic") color = true;
    }
    return color;
}

CoverageBlend& coverageBlend() {
    static CoverageBlend b;
    return b;
}

namespace {

// The samples a pixel of the target drawn into (the viewport's multisample anti-aliasing; 2 where
// the target is not known but anti-aliasing is on).
int targetSamples(const MHWRender::MDrawContext& context) {
    if (const MHWRender::MRenderTarget* target = context.getCurrentColorRenderTarget()) {
        MHWRender::MRenderTargetDescription d;
        target->targetDescription(d);
        return std::max(static_cast<int>(d.multiSampleCount()), 1);
    }
    return context.getPostEffectEnabled(MHWRender::MFrameContext::kAntiAliasing) ? 2 : 1;
}
bool multisampled(const MHWRender::MDrawContext& context) { return targetSamples(context) > 1; }

// The lineSamples each coverage fragment's shader instance (Maya's hair tube graph) was last given.
std::mutex& lineSamplesMutex() {
    static std::mutex m;
    return m;
}
std::map<const MHWRender::MShaderInstance*, int>& lineSamplesSet() {
    static std::map<const MHWRender::MShaderInstance*, int> set;
    return set;
}

}  // namespace

void coveragePreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance*) {
    CoverageBlend& b = coverageBlend();
    b.set = false;
    b.depthSet = false;
    MHWRender::MStateManager* states = context.getStateManager();
    if (!states || !colorPass(context)) return;
    bool blendItem = false;
    for (int i = 0; i < items.length() && !blendItem; ++i) {
        const MHWRender::MRenderItem* item = items.itemAt(i);
        blendItem = item && item->name() == kLodBlendItem;
    }
    if (blendItem) {
        if (!b.noDepthWrite) {
            MHWRender::MDepthStencilStateDesc desc;
            desc.setDefaults();
            desc.depthEnable = true;
            desc.depthWriteEnable = false;
            desc.depthFunc = MHWRender::MStateManager::kCompareLessEqual;
            b.noDepthWrite = MHWRender::MStateManager::acquireDepthStencilState(desc);
        }
        if (b.noDepthWrite) {
            b.keptDepth = states->getDepthStencilState();
            states->setDepthStencilState(b.noDepthWrite);
            b.depthSet = true;
            ++b.blendItemDraws;
        }
    }
    if (!b.blend) {
        MHWRender::MBlendStateDesc desc;
        desc.setDefaults();
        MHWRender::MTargetBlendDesc& t = desc.targetBlends[0];
        t.blendEnable = true;
        t.sourceBlend = MHWRender::MBlendState::kSourceAlpha;
        t.destinationBlend = MHWRender::MBlendState::kInvSourceAlpha;
        t.blendOperation = MHWRender::MBlendState::kAdd;
        t.alphaSourceBlend = MHWRender::MBlendState::kOne;
        t.alphaDestinationBlend = MHWRender::MBlendState::kInvSourceAlpha;
        t.alphaBlendOperation = MHWRender::MBlendState::kAdd;
        b.blend = MHWRender::MStateManager::acquireBlendState(desc);
    }
    ++(multisampled(context) ? b.sampled : b.hashed);
    if (!b.blend) return;
    b.kept = states->getBlendState();
    states->setBlendState(b.blend);
    b.set = true;
}

namespace {

// The lit lines' coverage fragment's draws: coveragePreDraw, and the target's samples a pixel handed
// to the fragment (lineSamples: one, stretches about four pixels long), set when they change.
void coverageSamplesPreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance* shader) {
    coveragePreDraw(context, items, shader);
    if (!shader) return;
    const int samples = targetSamples(context);
    {
        std::lock_guard<std::mutex> lock(lineSamplesMutex());
        auto [it, added] = lineSamplesSet().try_emplace(shader, samples);
        if (!added && it->second == samples) return;
        it->second = samples;
    }
    shader->setParameter("lineSamples", static_cast<float>(samples));
    shader->updateParameters(context);
}

}  // namespace

void coveragePostDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList&, MHWRender::MShaderInstance*) {
    CoverageBlend& b = coverageBlend();
    MHWRender::MStateManager* states = context.getStateManager();
    if (b.depthSet) {
        b.depthSet = false;
        if (states && b.keptDepth) states->setDepthStencilState(b.keptDepth);
    }
    if (!b.set) return;
    b.set = false;
    if (states && b.kept) states->setBlendState(b.kept);
}

namespace {

// The cards' draws (cardCoveragePreDraw, cardPassPreDraw): the alpha to coverage blend state (made
// once), the state each draw found (put back after), the color draws on a multisampled target and
// on one sample a pixel, and the value each card shader instance was last given (cardPass, or
// alphaToCoverage).
struct CardDraws {
    const MHWRender::MBlendState* blend = nullptr;
    const MHWRender::MBlendState* kept = nullptr;
    bool set = false;
    long long sampled = 0, tested = 0;
    std::mutex mutex;
    std::map<const MHWRender::MShaderInstance*, int> given;
};
CardDraws& cardDraws() {
    static CardDraws c;
    return c;
}

// `value` to the shader's `parameter` (and 1 - value to `inverse`'s) when it is not what the
// instance was last given.
void giveCardShader(MHWRender::MDrawContext& context, MHWRender::MShaderInstance* shader, const char* parameter, int value,
                    const char* inverse = nullptr) {
    CardDraws& c = cardDraws();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        auto [it, added] = c.given.try_emplace(shader, 0);
        if (it->second == value) return;
        it->second = value;
    }
    shader->setParameter(parameter, static_cast<float>(value));
    if (inverse) shader->setParameter(inverse, static_cast<float>(1 - value));
    shader->updateParameters(context);
}

}  // namespace

void cardCoveragePreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList&, MHWRender::MShaderInstance* shader) {
    CardDraws& c = cardDraws();
    c.set = false;
    MHWRender::MStateManager* states = context.getStateManager();
    const bool color = colorPass(context);
    if (states && color && multisampled(context)) {
        if (!c.blend) {
            // Blending off, as the opaque pass draws: the alpha only says which samples the card covers.
            MHWRender::MBlendStateDesc desc;
            desc.setDefaults();
            desc.alphaToCoverageEnable = true;
            c.blend = MHWRender::MStateManager::acquireBlendState(desc);
        }
        if (c.blend) {
            c.kept = states->getBlendState();
            states->setBlendState(c.blend);
            c.set = true;
        }
    }
    if (color) ++(c.set ? c.sampled : c.tested);
    // The alpha test (in the color fragment) off while the alpha says the samples. Only in the color
    // pass: Viewport 2.0 draws the cards' shadow maps with the graph too, and parameters updated
    // from a shadow pass's callback left the scene without shadows; there the graph keeps what the
    // last color draw left, the alpha test (cardCoveragePostDraw), so the shadows are cut out.
    if (shader && c.set) giveCardShader(context, shader, "alphaToCoverage", 1, "alphaTest");
}

void cardCoveragePostDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList&, MHWRender::MShaderInstance* shader) {
    CardDraws& c = cardDraws();
    if (!c.set) return;
    c.set = false;
    if (MHWRender::MStateManager* states = context.getStateManager(); states && c.kept) states->setBlendState(c.kept);
    if (shader) giveCardShader(context, shader, "alphaToCoverage", 0, "alphaTest");
}

void cardPassPreDraw(MHWRender::MDrawContext& context, MHWRender::MShaderInstance* shader) {
    if (!shader) return;
    int pass = 0;
    const MStringArray& semantics = context.getPassContext().passSemantics();
    for (unsigned i = 0; i < semantics.length(); ++i) {
        if (semantics[i] == MHWRender::MPassContext::kShadowPassSemantic) pass = 1;
        if (semantics[i] == MHWRender::MPassContext::kPointLightShadowPassSemantic) pass = 2;
    }
    CardDraws& c = cardDraws();
    if (colorPass(context)) ++(multisampled(context) ? c.sampled : c.tested);
    giveCardShader(context, shader, "cardPass", pass);
}

void noteCardShader(const MHWRender::MShaderInstance* shader) {
    CardDraws& c = cardDraws();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.given[shader] = 0;  // (a released instance's address may come again: the shader's default)
}

void cardCoverageDraws(long long& sampled, long long& tested) {
    sampled = cardDraws().sampled;
    tested = cardDraws().tested;
}

// The flat lines' effect (technique Plain: the item's color; Colored: the colors stream) with the
// lines' blending around its draws; null when it does not compile (said once: they draw opaque).
MHWRender::MShaderInstance* acquireLinesShader(const MHWRender::MShaderManager& shaders, bool colored) {
    static int state = 0;  // 0 not tried, 1 compiles, -1 failed
    if (state < 0) return nullptr;
    const char* text = linesEffectText();
    if (!text) return nullptr;
    MHWRender::MShaderInstance* shader = shaders.getEffectsBufferShader(
        text, static_cast<unsigned>(std::strlen(text)), colored ? "Colored" : "Plain", nullptr, 0, true, coveragePreDraw, coveragePostDraw);
    if (!shader) {
        state = -1;
        MGlobal::displayWarning(MString("GroomLab: the strand lines' shader did not compile, so they draw without width as coverage: ") +
                                MHWRender::MShaderManager::getLastError());
        return nullptr;
    }
    state = 1;
    return shader;
}

bool linesShaderAvailable(const MHWRender::MShaderManager& shaders) {
    static int state = 0;
    if (state == 0) {
        MHWRender::MShaderInstance* probe = acquireLinesShader(shaders, false);
        state = probe ? 1 : -1;
        if (probe) shaders.releaseShader(probe);
    }
    return state > 0;
}

void GroomSubSceneOverride::hairShaderDefaults(MHWRender::MShaderInstance& shader, int kind) {
    if (!(kind == 2 || kind == 6 || kind == 9 || kind == 10)) return;
    // Shadowed hair reads dark, not black. Cards the same: they shade with their strands' own
    // normals (cards.cpp: the strands' lit drawing's), from either side (kCardGeomFragment), so they
    // get the strands' light, and their ambient too. (The highlight is style()'s: the HairColor
    // node's.)
    const float ambient[3] = {0.06f, 0.06f, 0.06f};
    shader.setParameter("ambientColor", ambient);
    shader.setParameter("diffuse", 0.85f);
    shader.setParameter("specularShift", 6.0f);
    shader.setParameter("scatter", 0.15f);
    shader.setParameter("scatterPower", 5.0f);
    shader.setParameter("translucence", 0.25f);
    shader.setParameter("translucenceFocus", 0.5f);
    shader.setParameter("translucenceDepth", 0.5f);
    shader.setParameter("tubeDirection", 0);
    shader.setParameter("isSingleSided", false);
    if (MHWRender::MTexture* ramp = kind != 6 ? rootToTipTexture() : nullptr) {  // the cards' map is theirs
        MHWRender::MTextureAssignment assignment;
        assignment.texture = ramp;
        shader.setParameter("map", assignment);
    }
    MHWRender::MSamplerStateDesc samplerDesc;
    samplerDesc.filter = MHWRender::MSamplerState::kMinMagMipLinear;
    samplerDesc.addressU = samplerDesc.addressV = MHWRender::MSamplerState::kTexClamp;
    if (const MHWRender::MSamplerState* sampler = MHWRender::MStateManager::acquireSamplerState(samplerDesc)) {
        shader.setParameter("textureSampler", *sampler);
        if (kind == 6) shader.setParameter("coverageMapSampler", *sampler);
    }
    if (MHWRender::MTexture* ramp = kind == 6 ? rootToTipTexture() : nullptr) {
        // The card graph's coverage map before the maps are bound (bindCardTextures; never unbound:
        // OpenGL reads a null texture), the solid cards' (coverageSolid: not read).
        MHWRender::MTextureAssignment assignment;
        assignment.texture = ramp;
        shader.setParameter("coverageMap", assignment);
    }
}

MHWRender::MShaderInstance* GroomSubSceneOverride::acquireLitShader(const MHWRender::MShaderManager& shaders, int kind, bool coverage) {
    if (hairShading_) {
        // GroomLab's hair (hair_shading.h): its own draw callbacks (the lights; the coverage's too).
        if (!hairState_) hairState_ = std::make_shared<HairGroomState>();
        if (MHWRender::MShaderInstance* shader = acquireHairShader(shaders, kind == 9, coverage, hairState_)) return shader;
    }
    const MHWRender::MShaderInstance::DrawCallback pre = coverage ? coverageSamplesPreDraw : nullptr;
    const MHWRender::MShaderInstance::DrawCallback post = coverage ? coveragePostDraw : nullptr;
    MHWRender::MShaderInstance* shader = nullptr;
    if (kind == 9) {  // Maya's hair tube shading in the strands' own colors (the colors stream)
        shader = shaders.getFragmentShader("groomLabHairSurface", "outSurfaceFinal", true, pre, post);
    } else if (plainHairShaderRegistered()) {
        // Maya's hair tube shading (as paint effects tubes use): Lambert diffuse plus anisotropic
        // highlights along the strand, lit by the scene's lights with their shadows; its graph with the
        // specular fragment that takes no bitangent stream (the strands carry none).
        shader = shaders.getFragmentShader("groomLabHairTubeSurface", "outSurfaceFinal", true, pre, post);
    }
    if (shader) {
        // (A released instance's address may come again: its lineSamples is the fragment's default.)
        std::lock_guard<std::mutex> lock(lineSamplesMutex());
        lineSamplesSet()[shader] = 1;
        return shader;
    }
    return kind == 9 ? nullptr : shaders.getStockShader(MHWRender::MShaderManager::k3dBlinnShader);  // (no graph: a plain lit shader)
}

void GroomSubSceneOverride::style(MHWRender::MRenderItem& item, ItemBuffers& buffers, const float color[4],
                                  float lineWidth) {
    const int kind = buffers.shaderKind;
    if (kind == 0 || kind == 3) buffers.shader->setParameter("solidColor", color);
    if (kind == 6) {
        buffers.shader->setParameter("solidColor", color);  // solid cards (no maps) in the theme's card color
        const float white[3] = {1.0f, 1.0f, 1.0f};
        buffers.shader->setParameter("color", white);  // the hair graph's color multiplies the map: the map alone
    }
    if (buffers.hair) {
        // GroomLab's hair: the fiber's color (the item's, or the colors stream's), the pigment's look
        // or not, and HairColor's highlight.
        const float own[4] = {color[0], color[1], color[2], 1.0f};
        bool debugSet = false;
        const int debug = MGlobal::optionVarIntValue("groomLabHairDebug", &debugSet);
        setHairMaterial(*buffers.shader, own, shape_->hairLook(), floatPlug(GroomShape::aDisplayStrandVariation),
                        debugSet ? debug : 0);
    } else if (kind == 2 || kind == 6 || kind == 9 || kind == 10) {
        // The highlight the graph's HairColor node asks for: its strength scales a warm white,
        // its roughness spreads the lobe (a narrow one at 0, a broad soft one at 1).
        const HairLook look = shape_->hairLook();
        const HairHighlight highlight = hairHighlight(look);
        buffers.shader->setParameter("specularColor", highlight.color.data());
        buffers.shader->setParameter("specularPower", highlight.power);
        if (kind != 2) {
            // The structural color (how much rides in the colors' alpha: FeatherColor's iridescence),
            // its hue turned by the view's angle to each fiber in the fragment.
            buffers.shader->setParameter("iridescenceColor", look.iridescenceColor.data());
            buffers.shader->setParameter("iridescenceShift", look.iridescenceShift);
        }
    }
    if (kind == 7) {
        buffers.shader->setParameter("gColor", color);
        buffers.shader->setParameter("gUseVertexColor", buffers.vertexColors ? 1.0f : 0.0f);
        buffers.shader->setParameter("gLineWidth", std::max(lineWidth, 1.0f));
        buffers.shader->setParameter("gPull", displayFlag(GroomShape::aDisplayOverlaysOnTop) ? overlayPull_ : 0.0f);
    }
    if (kind == 11) {  // (the line width: the dots' size in pixels)
        buffers.shader->setParameter("gPointSize", std::max(lineWidth, 1.0f));
        buffers.shader->setParameter("gPull", displayFlag(GroomShape::aDisplayOverlaysOnTop) ? overlayPull_ : 0.0f);
    }
    if (kind == 3 || kind == 4) {
        const float width[2] = {lineWidth, lineWidth};
        buffers.shader->setParameter("lineWidth", width);
    }

    if (kind == 2 && !buffers.hair) {
        buffers.shader->setParameter("color", color);  // the Blinn fallback takes diffuseColor
        const float diffuse[4] = {color[0], color[1], color[2], 1.0f};
        buffers.shader->setParameter("diffuseColor", diffuse);
    }
    if ((kind == 9 && !buffers.hair) || kind == 10) {
        const float white[3] = {1.0f, 1.0f, 1.0f};
        buffers.shader->setParameter("color", white);  // the colors stream is the color
    }
    // Width as coverage: the strand lines' width scale (the ribbons') and floor, the density the
    // coverage is taken to the render's from (displayCoverageDensity: the preview's; else 1); the
    // lit graph's coverage on or off (ribbons draw with the same graph: off).
    DrawTimes::Shader& noted = currentDraw().shaders[item.name().asChar()];
    noted.instance = reinterpret_cast<std::uintptr_t>(buffers.shader);
    noted.kind = kind;
    noted.hair = buffers.hair;
    noted.coverage = buffers.coverage;
    if (buffers.hair) {
        // What the hair shader's draw callbacks take (its blending with coverage, its groom's lights):
        // this item's, every update, whatever another acquire of the same address left there.
        noteHairShader(buffers.shader, buffers.coverage, hairState_);
        noted.registered = hairShaderCoverage(buffers.shader);
    }
    if (buffers.coverage || kind == 2 || kind == 9) {
        const float widthScale = floatPlug(GroomShape::aRibbonWidthScale);
        const float lineDensity = displayFlag(GroomShape::aDisplayCoverageDensity) ? shape_->evaluatedDensity() : 1.0f;
        noted.widthScale = widthScale;
        noted.lineDensity = lineDensity;
        buffers.shader->setParameter("lineWidthScale", widthScale);
        buffers.shader->setParameter("lineCoverageFloor", kLineCoverageFloor);
        buffers.shader->setParameter("lineDensity", lineDensity);
        if (kind == 2 || kind == 9) buffers.shader->setParameter("lineCoverage", buffers.coverage ? 1.0f : 0.0f);
        // Every strand as it is, every fragment in one pass (the level of detail sets its own: updateLod).
        buffers.shader->setParameter("lineLod", 0.0f);
        buffers.shader->setParameter("lineBlend", 0.0f);
    }
    if (kind == 5) {
        const float diffuse[4] = {color[0], color[1], color[2], 1.0f};
        const float noSpecular[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        buffers.shader->setParameter("diffuseColor", diffuse);
        buffers.shader->setParameter("specularColor", noSpecular);
    }
    // Lit strands take part in shadows and ambient occlusion like any surface.
    // Receiving is optional: dense strands shadowing each other in a shadow
    // map speckle.
    const int shadows = kind == 2 || kind == 6 || kind == 9 || kind == 10 ? MPlug(object_, GroomShape::aDisplayShadows).asShort() : 0;
    // (Viewport 2.0 draws the cards' shadow maps with their own shaders, so the shadows are cut out
    // as the cards are: GroomLab's hair's card effect writes the depth there, cardPassPreDraw; Maya's
    // hair graph keeps its alpha test, cardCoveragePreDraw.)
    item.castsShadows(shadows >= 1);
    item.receivesShadows(shadows >= 2);
    // Hair, cards and scales take the viewport's ambient occlusion and the rest. Not the textured
    // cards: the ambient occlusion's normals and depth are drawn with a shader of Viewport 2.0's own
    // (it calls none of the item's there; on DirectX 11 it leaves GroomLab's hair's card effect out
    // of that pass, on OpenGL and for Maya's hair graph it draws the item), so their whole quads
    // would darken what is behind them in boxes; their maps carry the bake's ambient occlusion.
    const bool cutCards = kind == 6 && cardsTextured_;
    item.setExcludedFromPostEffects((kind != 2 && kind != 6 && kind != 9 && kind != 10) || cutCards);
    item.setMatrix(&MMatrix::identity);  // world space, whatever the groom's transform holds
}
}  // namespace groomlab::maya
