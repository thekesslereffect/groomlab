// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include <maya/MMatrix.h>
#include <maya/MObject.h>
#include <maya/MPxCommand.h>
#include <maya/MString.h>
#include <maya/MSyntax.h>

#include "groomlab/scales.h"

namespace groomlab::maya {

// The curve exporters evaluate the groom at each frame (in a DG context,
// without changing the scene time). Frames default to the current frame;
// density defaults to 1 (render density). They return the number of frames written.

#if GROOMLAB_HAS_ALEMBIC
// groomLabExportAlembic -file path [-startFrame f] [-endFrame f] [-step s] [-density d]
//                       [-guideGroup name | -guidePoses] [-scales mesh|none] groom
// Animated Alembic curves (OR-2): the strands (with their scales beside them as an animated
// mesh, alembic_io.h, unless -scales none), or with -guideGroup the
// hierarchy guides of a group (posed, e.g. for a simulation to start from),
// or with -guidePoses the guide poses themselves (simulated guides, for a
// groom file's guide cache). Guide curves carry the guide ID as strandId.
// For an engine's importer (Unity's hair package): -scale s multiplies points and widths,
// -pointsPerStrand n resamples every strand to n points, -vertexUVs writes the root UV
// per point (AlembicCurvesWriter::Options).
class ExportAlembicCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};
#endif

// groomLabExportUsd -file path.usda [-startFrame f] [-endFrame f] [-step s] [-density d]
//                   [-scales mesh|instancer|both|none] groom
// A USD BasisCurves prim, one time sample per frame (OR-1), with the scales as their bent mesh,
// a PointInstancer of their prototypes (the scene's meshes written into it), both or neither
// (usd_writer.h).
class ExportUsdCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};

// groomLabExportVrscene -file path.vrscene [-startFrame f] [-endFrame f] [-step s] [-density d]
//                       [-renderOverrides b] [-minPixelWidth w] groom
// A V-Ray scene (vrscene_writer.h): the strands as GeomMayaHair in the groom's object space, the
// hair shader (BRDFHair4 from the groom's look) and a Node with the groom's world matrix; frames as
// interpolate() keys. For V-Ray Standalone or V-Ray for Maya's VRayScene node.
class ExportVrsceneCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};

// groomLabExportGroom -file path.groom [-scalpCache abc [-scalpObject name]]
//                    [-guideCache abc [-guideObject name]] groom
// Writes a groom file at the current time: graph, guides and scalp, plus
// references to caches written separately (the Python export_groom writes the
// scalp cache with AbcExport). Relative cache paths are relative to the file.
class ExportGroomCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};

// groomLabExportGltf -file path(.glb|.gltf) [-name n] [-resolution r] [-occlusion ao]
//                    [-binary b] [-scalpCap b] [-capResolution r] [-scale s]
//                    [-alphaMode mask|blend|opaque] [-alphaCutoff c] [-rootToTip b] [-tangents b]
//                    [-levelOfDetail name -lodGraph json]... groom
// The groom's game cards as glTF 2.0 (gltf_writer.h): the cards as a bake makes them, with
// their maps baked in memory into the file (a .glb) or beside it (a .gltf), Y up, in meters
// (-scale, 0.01 from Maya's centimeters). Each -levelOfDetail makes a mesh <name>_<lod>
// from its -lodGraph (the groom's graph with that LOD's Cards settings); without, one mesh
// <name>_cards from the groom's own graph. -scalpCap (on) adds <name>_scalpCap, the scalp
// with its scalp bake, when the scalp has UVs. The groom and its bake are left alone.
// Returns the files written.
class ExportGltfCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};

// groomLabScaleMesh [-name n] groom
// The groom's scales at the current time, bent onto the skin (scales.h), as a Maya mesh under a
// new transform with the groom's matrix: what FBX takes to an engine (a static or skeletal mesh).
// UV sets: map1 the prototypes' uvs, scaleUV the skin's uv at each scale's root (per face),
// scaleData (scaleId, scaleVariant) per face; color sets: scaleColor (the strand's color along
// the scale, per vertex) and scaleRootColor (per face); normals locked as the scales shade.
// Undoable. Returns the transform.
class ScaleMeshCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return true; }

private:
    std::shared_ptr<const ScaleMesh> mesh_;
    MMatrix matrix_;
    MString name_;
    MObject transform_;
};

// groomLabBakeScaleMaps -directory d -name n [-resolution r] [-maxHeight h] [-padding p] groom
// The groom's scales at the current time baked onto its scalp's uvs (scale_maps.h):
// <d>/<n>_ScaleNormal.png (tangent space, OpenGL +Y), <n>_ScaleHeight.png (16-bit, 0 the skin
// to white -maxHeight; 0: the scales' highest point) and <n>_ScaleColor.png (sRGB, alpha the
// coverage), a tile each (<n>_<Map>.<tile>.png) when the scalp's uvs are UDIM tiles. Returns
// the files written, then the max height as text (the displacement's scale).
class BakeScaleMapsCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};

// groomLabBakeFollicleMap -directory d -name n [-resolution r] [-radius r] [-hardness h] [-padding p] groom
// A radial gradient round every root of the groom at the current time baked onto its scalp's uvs
// (follicle_map.h): <d>/<n>_FollicleMap.png (16-bit gray: white at a root, black past -radius;
// 0: from the roots' spacing), a tile each (<n>_FollicleMap.<tile>.png) when the scalp's uvs are
// UDIM tiles. Returns the files written, then the radius baked with as text.
class BakeFollicleMapCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    bool isUndoable() const override { return false; }
};
}  // namespace groomlab::maya
