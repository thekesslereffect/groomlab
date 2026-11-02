// SPDX-License-Identifier: Apache-2.0
#pragma once

// V-Ray scene export: a groom's strands, and its hair shader, as a .vrscene (V-Ray's text scene
// format), for V-Ray Standalone (`vray -sceneFile=hair.vrscene`, or #include it into a scene) and
// V-Ray for Maya's VRayScene node. Needs no V-Ray library: the format is text, the long lists in
// V-Ray's packed form (ListVectorHex("ZIPC...": zlib-compressed, base64), as V-Ray's own exporters
// write them). web/content/docs/other-renderers.mdx ("V-Ray") documents it for users.
//
// What one file holds, `<name>` the groom's name made a V-Ray plugin name:
//
//   SettingsUnitsInfo <name>@units  the scene unit (meters_scale), up axis, frames per second
//   GeomMayaHair <name>@hair        the strands, in the groom's object space:
//     num_hair_vertices               points per strand (strands of fewer than 2 points are left out)
//     hair_vertices                   the points
//     widths                          per point, the strand's RADIUS (V-Ray's hair width is a radius:
//                                     a width of 1 renders 2 units across; checked in vray)
//     colors                          per point, the hair's base color for BRDFHair4's dye
//                                     (vrayDyeColor of export_view.h's base color, the point's
//                                     when the color varies along the strand; darkened where wet)
//     transparency                    per point, 1 - opacity, when the strands have an opacity channel
//     strand_uvw                      per strand, the root's (u, v, 0) on the scalp, when it has UVs
//     geom_splines 1, min_pixel_width (Options)
//   TexHairSampler <name>@hairSampler   reads the colors (hair_color) and transparency at a hit
//   BRDFHair4 <name>@hairBrdf       VRayHairNextMtl's BRDF: melanin 0 (the pigment is in the
//                                   colors), dye_color the sampler's hair_color, the look's lobes
//                                   (vrayHairBrdf), trace_depth; glints, glitter and randomness off
//                                   (GroomLab varied each strand already)
//   MtlSingleBRDF <name>@hairMtl
//   Node <name>@node                the geometry, material and the object-to-world transform, its
//                                   scale moved into the points and widths (V-Ray does not scale
//                                   hair widths by a transform: a groom scaled 0.01 renders its
//                                   strands 100 times too thick; checked in vray); no nsamples
//                                   (V-Ray's default, motion blur's; 1 would hold the keys still)
//
// Animation: one file for a frame range. A value that changes between frames (the points on a
// moving groom; the counts, widths or colors when the strands change) is written as V-Ray's
// interpolate((frame, value), ...) keys, one per written frame; one that does not is written once.
// V-Ray renders a frame from its keys (`vray -frames=12`) and blurs motion between them
// (SettingsMotionBlur): to blur a frame, write the frame after it too (or subframes: step 0.5).
// Every frame should have the same strands and point counts (a rest mesh on a deforming scalp,
// as for Alembic and USD); V-Ray cannot interpolate between lists of different lengths.
//
// Not written: instances and scales (USD carries them), FeatherColor's iridescence (Arnold's
// network only), per-strand highlight variation (the specular, roughness and tilt channels: the
// look's values for the whole groom), cameras, lights and render settings (the file is an asset to
// include). The file is written to "<path>.tmp" and moved over `path` on close.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/hair_color.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class VrsceneError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace vray {

// ---- The look in BRDFHair4's terms ------------------------------------------------------------
//
// BRDFHair4 (VRayHairNextMtl) absorbs its dye color over no melanin as a hair color, as
// aiStandardHair absorbs its base color (Chiang et al. 2016's mapping): every GroomLab color, the
// pigment's too (hair_color.h melaninColor), reaches it as the dye, per point. Its lobes differ
// from aiStandardHair's, so the mapping is fitted: tests/data/app/vray_compare.py beside
// arnold_compare.py --linear-srgb (pigment groom: melanin through user data), the head groom
// (tests/data/app/head_graph.json, 100k strands) under the app's key light, four pigments
// (melanin/redness 0.35/0.2 brown, 0.12/0.45 blonde, 0.85/0.1 dark, 0.4/0.95 red) and Arnold
// roughness 0.14, 0.3 and 0.5, the hair's mean linear color and its blurred image compared:
//   - the dye: c's absorption (hairColorAbsorption) times kVRayDyeAbsorption, as a color again;
//   - glossiness kVRayGlossiness - kVRayGlossinessPerRoughness x Arnold's roughness (0.46 for the
//     default 0.3: BRDFHair4's glossiness 1 - roughness renders the hair below the light dark);
//   - softness kVRaySoftnessScale x the azimuthal roughness (0.35 for Arnold's 0.2);
//   - IOR kVRayIor: BRDFHair4's primary reflection is weaker than aiStandardHair's R lobe (black
//     hair, nearly all R, renders 0.7 of Arnold's at 1.55, 0.95 at 1.7; the primary tint does not
//     go above 1);
//   - trace_depth kVRayTraceDepth (V-Ray's 10 loses light in light hair: blonde 0.7 of Arnold's).
// Then brown, blonde and black come within about 10% of Arnold channel by channel under the key;
// red renders 20-30% brighter in red and green (BRDFHair4 carries more light through red hair).
// Under the app's studio lights (tools/bench/vray/vray_sheet.py): the hair_basics and
// cinematic_hair presets within 3%, blonde and a light RGB look 10-15% darker, red 20-30% brighter.
constexpr float kVRayDyeAbsorption = 0.9f;
constexpr float kVRayGlossiness = 0.63f, kVRayGlossinessPerRoughness = 0.56f;
constexpr float kVRaySoftnessScale = 1.75f;
constexpr float kVRayIor = 1.7f;
constexpr int kVRayTraceDepth = 40;
std::array<float, 3> vrayDyeColor(const std::array<float, 3>& baseColor);

// BRDFHair4's highlight for the look's lobes (hair_color.h hairLobes: aiStandardHair's), as above;
// the primary tint the R lobe's tint, the highlight shift the cuticle tilt in degrees.
struct HairBrdf {
    float glossiness = 0.5f;
    float softness = 0.5f;
    float primaryTint = 1.0f;
    float highlightShift = kHairShiftDegrees;
    float ior = kVRayIor;
    int traceDepth = kVRayTraceDepth;
};
HairBrdf vrayHairBrdf(const HairLobes& lobes);

// A V-Ray plugin name for `name`: letters, digits and '_' (others become '_'), not starting with
// a digit; "groom" when empty.
std::string pluginName(const std::string& name);

// The strands as GeomMayaHair takes them, what the file below holds and what GroomLab's V-Ray for
// Maya translator hands V-Ray: strands of fewer than 2 points left out, the points and radii
// times `scale`, the colors the dye (vrayDyeColor of the base color, darkened where wet).
struct HairArrays {
    std::vector<std::int32_t> counts;  // num_hair_vertices
    std::vector<float> xyz;            // hair_vertices, 3 a point
    std::vector<float> widths;         // a radius a point
    std::vector<float> colors;         // 3 a point
    std::vector<float> transparency;   // 3 a point (1 - opacity), when hasTransparency
    std::vector<float> uvw;            // strand_uvw, the root's (u, v, 0), 3 a strand, when hasUVs
    bool hasTransparency = false;      // the strands have an opacity channel
    bool hasUVs = false;               // the scalp gives the roots UVs
};
// `like`: another frame's arrays, whose hasTransparency and hasUVs this one keeps (zeros where
// these strands lack them), so every frame's parameters agree.
HairArrays hairArrays(const StrandSet& strands, const ScalpMesh* scalp, float scale = 1.0f,
                      const HairArrays* like = nullptr);

// The scale of a transform (16 values, row-major with the translation in the last row: Maya's
// MMatrix) that V-Ray does not apply to hair widths: an axis's length when the scale is uniform,
// else the cube root of the determinant's magnitude; 1 for none. A groom's radii are multiplied
// by it (and, in a .vrscene, its points, the Node's matrix divided by it).
double matrixScale(const double* objectToWorld);

}  // namespace vray

class VrsceneWriter {
public:
    struct Options {
        double framesPerSecond = 24.0;
        double metersPerUnit = 0.01;  // Maya's internal unit is the centimeter (the file's unit, after `scale`)
        // The points and radii multiplied by this (the strands in another unit: 0.01 puts a
        // centimeter groom in meters). Scale them here rather than with a transform: V-Ray does not
        // scale hair widths by a Node's or a VRayScene's transform (the strands render that much
        // thicker; checked in vray).
        double scale = 1.0;
        std::string upAxis = "Y";
        std::string comment;          // written into the file's header comment
        // The groom's look (hair_color.h hairLobes of GroomSession::hairLook()).
        HairLobes lobes;
        // V-Ray's GeomMayaHair min_pixel_width: strands thinner than this many pixels are drawn
        // this wide and more transparent (0: as they are, as Arnold's procedural by default).
        float minPixelWidth = 0.0f;
        // The long lists zlib-compressed (ZIPC, about 40% smaller), else plain hex.
        bool compress = true;
    };

    // `name` names the file's plugins (vray::pluginName).
    VrsceneWriter(const std::string& path, const std::string& name, const Options& options);
    ~VrsceneWriter();

    VrsceneWriter(const VrsceneWriter&) = delete;
    VrsceneWriter& operator=(const VrsceneWriter&) = delete;

    // Frames must increase. `scalp` supplies root UVs (strand_uvw). `objectToWorld`: the groom's
    // matrix (16 values, row-major with the translation in the last row: Maya's MMatrix), null
    // for the identity; the Node's transform, keyed like any value when it moves. Its scale (the
    // cube root of its determinant) goes into the points and radii instead and the Node keeps the
    // rest: V-Ray does not scale hair widths by a Node's transform.
    void writeFrame(double frame, const StrandSet& strands, const ScalpMesh* scalp = nullptr,
                    const double* objectToWorld = nullptr);
    std::size_t frameCount() const;
    // The look's lobes (Options::lobes), for a host that knows them only once it has evaluated
    // the groom: the shader is written on close.
    void setLobes(const HairLobes& lobes);

    // Assembles and writes the file. Called by the destructor if needed.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace groomlab
