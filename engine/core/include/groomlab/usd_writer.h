// SPDX-License-Identifier: Apache-2.0
#pragma once

// USD export (OR-1): strands as a BasisCurves prim in a text (.usda) layer,
// one time sample per frame. Needs no USD library; usdview, Maya, Houdini and
// any USD reader load it (convert to .usdc with usdcat when size matters: the
// text form is large, about 60 bytes per point per frame).
//
// The layout is a public contract, versioned: web/content/docs/usd-layout.mdx describes every
// prim, attribute and primvar, and tests/engine/core/test_usd_layout.cpp holds the writer to it.
// Renaming or retyping anything below, or changing a meaning, raises kUsdLayoutVersion (and the
// page); adding a primvar does not. The layer's customLayerData and the root prim's customData
// carry {groomlab: {layoutVersion, version}}.
//
//   /<name>            Xform, the default prim
//   /<name>/strands    BasisCurves, linear, nonperiodic
//     points, curveVertexCounts, widths (vertex), extent
//     primvars:strandId     (int64, uniform)   stable strand IDs
//     primvars:st           (uniform)          root UV on the scalp, when it has UVs
//     primvars:displayColor (color3f, uniform) linear RGB: HairColor's RGB, else from melanin, when present
//     primvars:<name>       (uniform/vertex)   the strands' channels, e.g. clumpId
//   /<name>/instances  PointInstancer of the Instance nodes' instances along the strands, when
//                      there are some: protoIndices, positions, orientations, scales, ids; its
//                      prototypes (instances/prototypes/<name>) a Mesh each for the built-in
//                      plates and the meshes Options::prototypes finds, else an Xform to reference
//                      geometry into
//   Scales (a Scales node, scales.h), as Options::scales asks:
//   /<name>/scales     Mesh, the scales bent onto the skin (exact: what the viewport and Arnold
//                      show), triangles, no subdivision; points, normals (vertex), primvars:st
//                      (vertex: the prototypes' uvs), primvars:displayColor and scaleColor (vertex:
//                      the strand's color along the scale), and per face primvars:scaleRootColor,
//                      scaleId (0-1, random per scale), scaleUV (the skin's uv at its root) and
//                      scaleVariant (which of its node's prototypes)
//   /<name>/scaleInstances  PointInstancer of the scales' prototypes, each placed by its scale's
//                      frame (tilted and raised as the mesh's, not bent onto the skin): light and
//                      editable in an engine (Unreal imports it as instanced static meshes), with
//                      per instance primvars:scaleRootColor, scaleId, scaleUV, scaleVariant
//   The strands only laying scales out (InstanceSet::hidesStrands) are written invisible.
//
// What is exported is described by export_view.h, shared with the Alembic,
// Unreal and Arnold writers. Channel names become USD identifiers; two that
// collide (a-b and a_b, or a point channel beside a strand channel of its name: colorR and
// colorR_2), or one that is reserved (usdChannelNames), get a numbered suffix (a_b_2). The layer
// is written to "<path>.tmp" and moved over `path` on close.

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {
struct HairLook;  // groom_session.h
}

namespace groomlab {

class UsdError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The layout's version (web/content/docs/usd-layout.mdx), written as groomlab.layoutVersion.
constexpr int kUsdLayoutVersion = 1;

// USD's metersPerUnit for a groom file's unit (GroomFile::units): "m", "mm", "in", "ft", else
// centimeters (0.01).
double usdMetersPerUnit(const std::string& unit);

class UsdCurvesWriter {
public:
    struct Options {
        double framesPerSecond = 24.0;
        double metersPerUnit = 0.01;  // Maya's default unit is the centimeter
        std::string upAxis = "Y";
        std::string comment;          // written into the layer's doc string
        // How scales are written: their bent mesh (exact), a PointInstancer of their prototypes
        // (light; loses the bend), both, or neither.
        enum class Scales { Mesh, Instancer, Both, None };
        Scales scales = Scales::Mesh;
        // The host's prototype meshes by name, written as the instancers' prototypes (the
        // built-in plates need none); a name it does not find is a placeholder Xform.
        PrototypeLookup prototypes;
        // With the groom's look (GroomSession::hairLook), the curves also carry the RenderMan
        // procedural's primvars (renderman::curvePrimvars: rman_*, hair_index), so GroomLab's
        // RenderMan hair network (PxrPrimvar readers) shades them as in RIS: RenderMan in Maya's
        // Hydra viewport (XPU lookdev). Null: not written.
        std::shared_ptr<const HairLook> rendermanLook;
    };

    // `name` becomes the root prim's name (made a valid USD identifier).
    UsdCurvesWriter(const std::string& path, const std::string& name, const Options& options);
    ~UsdCurvesWriter();

    UsdCurvesWriter(const UsdCurvesWriter&) = delete;
    UsdCurvesWriter& operator=(const UsdCurvesWriter&) = delete;

    // Frames must increase. `scalp` supplies root UVs (primvars:st); the
    // channels (and color) of the first frame are written on every frame.
    // `scales`: the strands' scales (buildScaleMesh of these strands on this scalp), when they
    // have some; the first frame's decide whether scales are written (a later frame without
    // writes none).
    void writeFrame(double frame, const StrandSet& strands, const ScalpMesh* scalp = nullptr,
                    const ScaleMesh* scales = nullptr);
    std::size_t frameCount() const;

    // Assembles and writes the layer. Called by the destructor if needed.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A valid USD prim name: letters, digits and '_', not starting with a digit.
std::string usdIdentifier(const std::string& name);

// An instance's frame (rows: the images of the prototype's axes, scaled; InstanceSet::Placed's
// layout) as USD's PointInstancer takes it: a unit quaternion (w, x, y, z) and a scale per axis.
struct UsdOrientation {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3f scale{1.0f, 1.0f, 1.0f};
};
UsdOrientation usdOrientation(const Vec3f& x, const Vec3f& y, const Vec3f& z);

}  // namespace groomlab
