// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class AlembicError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Writes strands as animated Alembic curves (OR-2).
//
// Layout: /<name> (xform, identity) / <name>Shape (curves). Curves are linear
// and non-periodic. Per-point "width" holds strand widths (diameters, scene
// units). Arbitrary geometry parameters, all per curve (uniform), mirror the
// USD writer (export_view.h): "strandId" (uint64) carries stable strand IDs so
// downstream tools can match strands across frames; every strand channel
// (float) by its name, e.g. "clumpId"; and "Cd" (rgb, linear): HairColor's
// colorR/G/B channels, else from the HairColor melanin channels when the
// strands have them. The native curves
// "uvs" param holds the root UV on the scalp (per curve) when a scalp with
// UVs is given. The first frame decides which channels every frame carries.
// Topology may change between frames. Frames are uniformly spaced by
// `secondsPerFrame` from `startSeconds`.
//
// The archive is written to "<path>.tmp" and moved over `path` on close.
//
// Options fit the file to an engine's importer (Unity's hair package reads
// Alembic curves through com.unity.formats.alembic): `scale` multiplies the
// points and widths (scene units to the engine's), `pointsPerStrand` (2 or
// more) resamples every strand to that many points evenly by arc length (see
// exportCopy in export_view.h), and `vertexUVs` writes the root UV once per
// point (vertex scope, repeated along each strand) instead of once per curve:
// Unity's importer reads "uvs" per point and drops a curve set whose UVs are
// fewer than its points. `positiveRootWidths` keeps every strand's root width
// above 0 (keepRootWidthsPositive): Unity's hair package drops a curve set's
// widths when any root width is 0 or less. `pointChannels` writes the point
// channels too, per vertex (a simulation bake's wetness, sim_cache.h), which
// AlembicCurvesReader reads back.
//
// writeGroups writes groom groups (Unity's hair package makes a strand group of
// each curves object): one group the layout writeFrame gives; several each
// /<name>/<group>/<group>Shape (the group's name made an object name), in the
// order given, each resampled to its own `pointsPerStrand` (0: the option's).
// Each point's velocity, scene units per second, for motion blur: a central difference of the
// strands a frame before and after (`before` or `after` null at an end of the range: one-sided),
// `seconds` apart from `now` each. A strand is matched by ID with the same point count; a strand
// not in a neighbour (or with other points) is still there (zero).
std::vector<Vec3f> strandVelocities(const StrandSet& now, const StrandSet* before, const StrandSet* after, double seconds);

class AlembicCurvesWriter {
public:
    struct Options {
        float scale = 1.0f;
        std::size_t pointsPerStrand = 0;  // 0: as they are
        bool vertexUVs = false;
        bool positiveRootWidths = false;
        bool pointChannels = false;  // also every point channel (float, per vertex) by its name
    };

    AlembicCurvesWriter(const std::string& path, const std::string& name, double startSeconds,
                        double secondsPerFrame);
    AlembicCurvesWriter(const std::string& path, const std::string& name, double startSeconds,
                        double secondsPerFrame, const Options& options);
    ~AlembicCurvesWriter();

    AlembicCurvesWriter(const AlembicCurvesWriter&) = delete;
    AlembicCurvesWriter& operator=(const AlembicCurvesWriter&) = delete;

    // `scalp` supplies the root UVs; null writes none. `scales`: the strands' scales
    // (buildScaleMesh of these strands on this scalp), written beside the curves as an animated
    // polygon mesh, /<name>/scales/scalesShape: the bent triangles (wound as AbcExport winds a
    // Maya mesh), normals and the prototypes' uvs per corner, and as arbitrary params scaleColor
    // (rgb per vertex: the strand's color along the scale) and per face scaleRootColor (rgb),
    // scaleId (float, 0-1 random per scale), scaleUV (the skin's uv at the root) and
    // scaleVariant (int32). The first frame decides: with scales, a later frame without writes
    // an empty mesh; without, later scales are left out. Strands that only lay scales out
    // (InstanceSet::hidesStrands) are written hidden.
    //
    // `velocities`: each point's velocity (scene units per second, strandVelocities), written as
    // the curves' .velocities for a renderer's motion blur (scaled with Options::scale); left out
    // when resampled (Options::pointsPerStrand) or when its count is not the points'.
    void writeFrame(const StrandSet& strands, const ScalpMesh* scalp = nullptr, const ScaleMesh* scales = nullptr,
                    const std::vector<Vec3f>* velocities = nullptr);

    // A group of strands written as a curves object of its own. The first frame
    // decides the objects: a later frame's group of another name is left out, and
    // a group missing from a frame writes no curves. Names must differ.
    struct Group {
        std::string name;
        const StrandSet* strands = nullptr;
        std::size_t pointsPerStrand = 0;  // 0: Options::pointsPerStrand
    };
    void writeGroups(const std::vector<Group>& groups, const ScalpMesh* scalp = nullptr);
    std::size_t frameCount() const;
    // The curves objects' full paths, in order ("/<name>/<name>Shape", or one per
    // group); empty before the first frame and after close.
    std::vector<std::string> objectPaths() const;

    // Flushes, closes and moves the archive into place. Called by the destructor if needed.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes a groom for Unreal Engine's groom importer (its Alembic groom
// conventions, schema 1.5): /<name> (xform) with a "strands" curves object
// and, when guides are given, a "guides" curves object (groom_guide = 1, the
// guides Unreal simulates). Both carry groom_version_major/minor,
// groom_group_name, groom_width (per point) and, per curve: groom_id (a dense
// index 0..N-1 in export order: Unreal wants a non-negative int, so the 64-bit
// strand ID goes in groomlab_strand_id_low/high as two uint32), groom_group_id
// (the "groupId" strand channel when there is one, else 0), groom_roughness
// (a float per curve from the "roughness" strand channel, clamped to 0-1,
// when present), groom_clump_id (the "clumpId" channel, when present) and,
// for strands, groom_root_uv when the scalp has UVs; and per vertex
// groom_color (V3f linear RGB, the scope Epic's schema gives it: HairColor's
// colorR/G/B point channels when it wrote them, else its strand channels or
// the HairColor melanin color repeated along each strand; see strandColors in
// export_view.h), and groom_iridescence (float, FeatherColor's iridescence channel)
// when the strands carry one. One frame is a static groom; more make a groom
// cache. Written to "<path>.tmp" and moved into place on close.
//
// Space: Unreal's importer takes every point as its own (Z up, left-handed,
// centimeters), turned only by its conversion settings, which default to
// Rotation 0 and Scale 1 (AlembicHairTranslator.cpp, GroomSettings.h). So the
// points (strands and guides) and widths are written in Unreal's space, the way
// Unreal's FBX importer brings a mesh in from the same scene (FbxMainImport.cpp
// ConvertScene to Z up, then FbxUtilsImport.cpp ConvertPos flipping Y): a Y-up
// point (x, y, z) becomes (x, z, y), a Z-up one (x, -y, z), times the
// centimeters per unit (setSourceSpace; Y up and centimeters by default, the
// groom space of Maya, the app and the Blender helper). Both are mirror images
// (Unreal is left-handed), as the FBX mesh's are, so the groom lands on the
// head with the importer's defaults; curves have no winding to flip. The xform
// stays the identity; root UVs, colors and IDs are as they are.
class UnrealGroomWriter {
public:
    UnrealGroomWriter(const std::string& path, const std::string& name, const std::string& groupName,
                      double startSeconds, double secondsPerFrame);
    ~UnrealGroomWriter();

    // The groom's space (before the first frame): its up axis, "Y" or "Z", and its unit in
    // centimeters (1 for centimeters, 100 for meters). Throws std::invalid_argument for another
    // axis or a scale of 0 or less, AlembicError after the first frame.
    void setSourceSpace(const std::string& upAxis, double centimetersPerUnit);

    // The iridescence written as groom_ao too (before the first frame): Unreal's Hair Attributes
    // node gives a material the groom's AO, not attributes of its own, so a material reading the
    // mask takes it there (docs: the Unreal hair material recipe).
    void setIridescenceAsAo(bool on);

    UnrealGroomWriter(const UnrealGroomWriter&) = delete;
    UnrealGroomWriter& operator=(const UnrealGroomWriter&) = delete;

    // `guides` may be null (no guides object, or none this frame).
    void writeFrame(const StrandSet& strands, const StrandSet* guides, const ScalpMesh* scalp);

    // Groom groups (Unreal gives each its own material, LODs and physics; see
    // splitGroomGroups): each group is its own curves object with groom_group_id
    // = its id on every curve and groom_group_name = its name, and its guides (if
    // any) a guides object with the same id. One group writes "strands" and
    // "guides" as writeFrame does; several write "strands_<name>" and
    // "guides_<name>". groom_id stays dense and unique over the whole groom
    // (strands numbered group after group; guides likewise among themselves).
    // The first frame decides the groups: a later frame's group the first did
    // not have is left out, and a group missing from a frame writes no curves.
    struct Group {
        int id = 0;
        std::string name;
        const StrandSet* strands = nullptr;
        const StrandSet* guides = nullptr;  // null: none
    };
    void writeGroups(const std::vector<Group>& groups, const ScalpMesh* scalp);
    std::size_t frameCount() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AlembicCurvesFrame {
    double seconds = 0.0;
    StrandSet strands;  // positions, widths and IDs (IDs are 0.. when absent)
    // The curves' UVs as stored (one per strand, or per point with vertexUVs); empty without.
    std::vector<std::array<float, 2>> uvs;
    std::vector<Vec3f> velocities;  // the curves' .velocities as stored (one per point); empty without
};

// Reads every sample of the first curves object named `name` + "Shape", or
// of the first curves object in the archive when `name` is empty. A sample
// whose vertex counts do not fit its positions is an AlembicError.
std::vector<AlembicCurvesFrame> readAlembicCurves(const std::string& path, const std::string& name = {});

// Reads all curves in an archive at any time, for use as guides (GH-8); the
// archive stays open between calls, so a cache read frame after frame (or at
// every motion key) is opened once.
//
// Every curves object is included, or only those under the object named
// `objectName` (any depth) when it is set. Points are flattened to world
// space through parent xforms. Each object is sampled at its time sample
// nearest `seconds`; between two samples with the same curves the positions
// blend linearly. Curves of all objects are concatenated.
//
// IDs come from a "strandId" param when present, else from a hash of the
// object's full path and the curve index. Float params per vertex become
// point channels of their name (0 on the curves of objects without them). Non-linear curves (e.g. cubic
// from Maya's AbcExport) are read as their control points.
class AlembicCurvesReader {
public:
    explicit AlembicCurvesReader(const std::string& path, const std::string& objectName = {});  // throws AlembicError
    ~AlembicCurvesReader();

    AlembicCurvesReader(const AlembicCurvesReader&) = delete;
    AlembicCurvesReader& operator=(const AlembicCurvesReader&) = delete;

    StrandSet at(double seconds) const;  // throws AlembicError
    // The first and last sample times over the curves objects read (a timeline's range); false
    // when every object has one sample (nothing moves: `start` and `end` are left alone).
    bool timeRange(double* startSeconds, double* endSeconds) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// AlembicCurvesReader(path, objectName).at(seconds): one read.
StrandSet readAlembicCurvesAt(const std::string& path, double seconds, const std::string& objectName = {});

// An animated polygon mesh's vertex positions in world space (parent
// transforms applied), e.g. a scalp cache written by Maya's AbcExport.
// Between samples, positions are interpolated linearly (motion blur).
class AlembicMeshReader {
public:
    // The first mesh named `objectName`, or under an object of that name;
    // the first mesh when empty. Throws AlembicError.
    explicit AlembicMeshReader(const std::string& path, const std::string& objectName = {});
    ~AlembicMeshReader();

    AlembicMeshReader(const AlembicMeshReader&) = delete;
    AlembicMeshReader& operator=(const AlembicMeshReader&) = delete;

    const std::string& meshName() const;
    // False when the positions and every parent transform have one sample:
    // pointsAt then returns the same points at every time.
    bool animated() const;
    // The first and last sample times of the positions (or of a moving parent transform): what
    // a timeline spans. False when nothing moves (`start` and `end` are left alone).
    bool timeRange(double* startSeconds, double* endSeconds) const;
    std::vector<Vec3f> pointsAt(double seconds) const;  // throws AlembicError
    // The whole mesh at a time: pointsAt(seconds) on the mesh's faces (polygons triangulated as
    // fans from their first corner, in face order) with its UVs per corner when it has some
    // (face-varying, or one per vertex). A scalp for a new groom made on a mesh from a cache (the
    // standalone app); a groom file's own scalp carries the topology for the frames of its cache.
    // Throws AlembicError (a face index out of range, a face with fewer than 3 corners).
    std::shared_ptr<const ScalpMesh> meshAt(double seconds) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes an animated polygon mesh (a scalp cache the groom file references, as Maya's AbcExport
// writes one): /<name> (xform, identity) / <name>Shape (polymesh), the faces of the first frame's
// mesh on every frame (a scalp keeps its topology; AlembicMeshReader::meshAt reads it so), with
// its UVs per corner (face-varying) when it has some, and each frame's points. Frames are
// `secondsPerFrame` apart from `startSeconds`. The faces are the mesh's triangles, wound as
// AbcExport winds them (reversed), so the reader's reversed fan gives them back as they were.
// Written to "<path>.tmp" and moved into place on close. The standalone app's tests and
// benchmarks make deforming scalps with it; a host without Maya may cache a scalp of its own.
class AlembicMeshWriter {
public:
    AlembicMeshWriter(const std::string& path, const std::string& name, double startSeconds, double secondsPerFrame);
    ~AlembicMeshWriter();
    AlembicMeshWriter(const AlembicMeshWriter&) = delete;
    AlembicMeshWriter& operator=(const AlembicMeshWriter&) = delete;

    // Throws AlembicError for a point count other than the first frame's.
    void writeFrame(const ScalpMesh& mesh);
    std::size_t frameCount() const;
    void close();  // called by the destructor if needed

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace groomlab
