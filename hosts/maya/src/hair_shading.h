// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab's own hair shading in Viewport 2.0 (displayHairShading 1, the default): the lit strand
// lines and ribbons drawn with an effect of GroomLab's (HLSL for DirectX 11, OGSFX for the OpenGL
// Core Profile) instead of Maya's hair tube shading graph.
//
// The shading is the standalone app's lookdev hair, one source: engine/gpu/shaders/hair_bsdf.glsl,
// embedded at build time (Marschner's R, TT and TRT lobes in Karis 2016's closed forms, their widths
// from the roughness through d'Eon's fits, the fiber's absorption from the strands' hair color as
// aiStandardHair absorbs its base color, Zinke's dual scattering), calibrated against Arnold's aiStandardHair
// (tests/data/app/arnold_compare.py). Its lights are
// Viewport 2.0's own, read from the draw context before each draw (hairPreDraw: directional, point,
// spot and area lights, ambient lights; their color, intensity, decay and cone; light linking from
// the groom's ignored lights), so the hair lights as the scene does. The strand lines keep their
// width as coverage (the lines effect's hash, a coverage mask on multisampled targets) and the
// level of detail's uniforms (lineLod, lineBlend). Output is linear, scaled by pi as Viewport 2.0's
// own surfaces are (a Lambert there shows color x light), so the hair sits with them; Maya's color
// management turns it to the view.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <maya/MDagPath.h>
#include <maya/MShaderManager.h>

#include <maya/MApiNamespace.h>  // Maya's classes declared (MHWRender's are using-declarations of a
                                  // versioned namespace's: a plain forward declaration conflicts, Clang/GCC)

namespace groomlab {
struct HairLook;
}

namespace groomlab::maya {

// An OGSFX effect's text for this device. macOS's OpenGL stops at 4.1 (core profile): an effect
// saying #version 430 is taken there and then draws nothing, so it says 410 (what it uses is 4.1's).
inline std::string glslForDevice(std::string text) {
#ifdef __APPLE__
    if (text.compare(0, 12, "#version 430") == 0) text.replace(9, 3, "410");
#endif
    return text;
}

// The coverage's draw callbacks (groom_draw_override_shaders.cpp): the blend and depth states of the strand
// lines' draws in a color pass.
void coveragePreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance*);
void coveragePostDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance*);
// The cards' draw callbacks (groom_draw_override_shaders.cpp). Maya's hair graph's cards
// (cardCoveragePreDraw, cardCoveragePostDraw): in a color pass on a multisampled target (the
// viewport's multisample anti-aliasing), a blend state with alpha to coverage, so the alpha the
// card graph writes (its coverage) becomes the share of the pixel's samples it covers, and the
// graph's alphaToCoverage on (else off: the alpha test). GroomLab's hair's (technique HairCard,
// cardPassPreDraw: its coverage mask is its own): the pass it draws in handed to it (cardPass: 1 a
// shadow map, 2 a point light's, 0 any other), as Viewport 2.0 draws the cards' shadows with it.
// noteCardShader: a card shader just acquired (its value the shader's default, 0).
void cardCoveragePreDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance* shader);
void cardCoveragePostDraw(MHWRender::MDrawContext& context, const MHWRender::MRenderItemList& items, MHWRender::MShaderInstance*);
void cardPassPreDraw(MHWRender::MDrawContext& context, MHWRender::MShaderInstance* shader);
void noteCardShader(const MHWRender::MShaderInstance* shader);
// The cards' color draws on multisampled targets and on one sample a pixel (tests: groomLabGraphStats).
void cardCoverageDraws(long long& sampled, long long& tested);
// Whether a draw is in a pass drawing the scene's colors (not shadows, depth, selection...).
bool colorPass(const MHWRender::MDrawContext& context);
// The scene's rendering space against linear Rec.709 (sRGB's primaries), row-major. GroomLab's
// colors in Maya are linear values in the rendering space (as Maya's own color attributes and
// Arnold's are) and an sRGB map holds Rec.709 ones: the card and scalp bakes convert to Rec.709
// (toRec709), and a file node reads the map back into the rendering space, as GroomLab's card
// shading does (fromRec709: its cardFromRec709 rows). By the rendering space's name
// (colorManagementPrefs): ACEScg, ACES2065-1, P3 D65 and Rec.2020; identity for any other (linear
// Rec.709) and with color management off.
struct RenderingSpace {
    std::array<float, 9> toRec709, fromRec709;
};
RenderingSpace renderingSpace();
// The strand lines' width as coverage (engine/gpu/shaders/line_coverage.glsl, embedded: the app's
// too) in an effect's words: HLSL's for DirectX 11, else GLSL's as they are.
const std::string& lineCoverageText(bool hlsl);

// What a groom's hair shaders share from draw to draw: the lights it ignores (light linking) and
// its density grid (self-shadowing).
struct HairGroomState {
    std::vector<MDagPath> ignoredLights;
    // The hair's density grid (line_frames.h runDensity, written by the GPU after each update that
    // moved the strands): its texture (rg: the optical density, the ambient occlusion), the first
    // cell's corner, the cell's size and the cells along each axis; `valid` once written.
    MHWRender::MTexture* volume = nullptr;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float cell = 1.0f;
    unsigned size[3] = {0, 0, 0};
    bool valid = false;
    double gpuMs = -1.0;     // the last grid's milliseconds on the GPU (-1: not known)
    std::uint64_t serial = 0;  // grids written so far
    std::uint64_t volumeSerial = 0;  // textures made so far (volumeFor; the one now: this number)
    bool remake = false;             // the next volumeFor makes a new texture whatever its size
    // The texture as the hair shaders sample it, checked against the grid the kernels made (the
    // draw override's updateHairGrid: after a grid not made for moving strands, and in a new
    // texture): its texels' sum and the grid's (-1: not read), the texels that differed when last
    // checked, the grid's serial then, and how often the texture was written again from the CPU
    // because the device's copy had not landed.
    double textureSum = -1.0, ownSum = -1.0;
    std::size_t texelsDiffering = 0;
    std::uint64_t checkedSerial = 0;
    long long reuploads = 0;
    long long handleSwaps = 0;       // checks that found the texture on another device resource than the one written
    // What the last grid was made from (groomLabGraphStats -hairShading): the weight of a point of
    // width 1, the preview density it was taken to the render's from, the points splatted, the
    // strands' version, and the draw override's update it was made in.
    float pieceWeight = 0.0f;
    float density = 1.0f;
    std::size_t points = 0;
    std::uint64_t strandsVersion = 0;
    // A grid made in an update that wrote the strand lines is made again in the next update (once
    // the lines are on the device as drawn: a buffer's new contents may reach it after the update).
    bool settle = false;
    std::string why;         // why there is none ("": there is, or none was asked for)
    HairGroomState() = default;
    HairGroomState(const HairGroomState&) = delete;
    HairGroomState& operator=(const HairGroomState&) = delete;
    ~HairGroomState();
    // The texture for a grid of `size` cells (made again for another size); null where none can be made.
    MHWRender::MTexture* volumeFor(const unsigned size[3]);
};

// The grid on a hair shader: its texture, where it is, and how opaque the hair is to its own light
// (displaySelfShadowing; 0, or no valid grid: no shadow, no occlusion).
void setHairGrid(MHWRender::MShaderInstance& shader, const HairGroomState* groom, float selfShadowing);

// The hair effect: technique Hair (the item's color, hairColor) or HairColored (the colors stream),
// or with `card` HairCard (game cards: the baked color map cut out by its coverage, parameters as
// the cards' own effect names them: map, cutoff, solid, solidColor, mapSize), with hairPreDraw and
// hairPostDraw (the lights; with `coverage` the strand lines' blending too). Null where it does not
// compile (said once) or the draw API takes none (legacy OpenGL).
MHWRender::MShaderInstance* acquireHairShader(const MHWRender::MShaderManager& shaders, bool colored, bool coverage,
                                              const std::shared_ptr<HairGroomState>& groom, bool card = false);
bool hairShaderAvailable(const MHWRender::MShaderManager& shaders);
// What a hair shader's draw callbacks know it by, set again for the item drawing with it (style(),
// every update): its groom and whether it draws the strand lines' coverage (their blending). An
// instance's address may be another acquire's too (a released one taken again, a probe, an instance
// the shader manager hands out twice): the item's own state is what its draws take. Returns the
// coverage it was known by before (-1: not known).
int noteHairShader(const MHWRender::MShaderInstance* shader, bool coverage, const std::shared_ptr<HairGroomState>& groom);
// The coverage a hair shader's callbacks know it by (-1: not one of them); the draws whose
// instance they did not know (drawn with no coverage blending, no groom's lights).
int hairShaderCoverage(const MHWRender::MShaderInstance* shader);
// The grid a hair shader was last given (setHairGrid): its texture's number (HairGroomState
// volumeSerial; 0: none) and the grid's (serial). False: not one of the hair shaders.
bool hairShaderGrid(const MHWRender::MShaderInstance* shader, std::uint64_t* volume, std::uint64_t* serial);
long long hairDrawsUnknown();

// The hair's material on a hair shader: the color (its own, or the colors stream's: a hair color,
// hair_color.h, absorbed as aiStandardHair absorbs its base color) and HairColor's highlight
// (specular, roughness, tilt) as create_hair_shader gives it to aiStandardHair (core's hairLobes,
// hair_color.h). variation: the viewport's own per-strand variation (displayStrandVariation); debug
// (the optionVar groomLabHairDebug): 1 the tangents as colors, 2 each strand's random numbers, 3 the
// structural color's hue turned by each strand's random number.
void setHairMaterial(MHWRender::MShaderInstance& shader, const float color[4], const HairLook& look, float variation = 1.0f,
                     int debug = 0);

// The lights the groom at `path` ignores (light linking: MLightLinks), each light's path.
std::vector<MDagPath> ignoredLights(const MDagPath& path);

// Test hooks: the lights the last hair draw took (JSON: count, ambient, and each light's kind,
// direction or position, irradiance) and the hair draws so far.
std::string hairLightsJson();

}  // namespace groomlab::maya
