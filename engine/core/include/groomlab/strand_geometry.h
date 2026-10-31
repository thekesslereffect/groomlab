// SPDX-License-Identifier: Apache-2.0
#pragma once

// Strands as plain geometry for hosts: which strands to keep under a budget, and the strands
// as one polygon mesh (tubes or flat ribbons) with UVs. Used by the Maya utilities that turn a
// groom into curves or a mesh (groomLabConvert).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/feathers.h"
#include "groomlab/scales.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class ParamSet;
class ScalpMesh;
using StrandsPtr = std::shared_ptr<const StrandSet>;

// At most `maxCount` strands of `strands`, by ID: the strands whose hashed IDs are lowest,
// the ones a lower preview density keeps longest (keepAtDensity), so a smaller budget keeps a
// subset of a larger one's and nothing depends on the strands' order. Indices ascending; every
// strand when there are no more than maxCount.
std::vector<std::size_t> thinStrandsById(const StrandSet& strands, std::size_t maxCount);

struct StrandMeshOptions {
    // What the mesh is made of (a ToMesh node's `make`): Auto picks by what the strands carry
    // (scales, else other instances, else cards, else feathers with a rachis, else the strands);
    // Strands: tubes or ribbons round every strand; Scales: the scales' plates bent onto the skin
    // (scales.h buildScaleMesh); Feathers: the barbs as tubes or ribbons and the rachis tubes
    // (buildRachisMesh); Cards: the Cards nodes' cards (their quads, laid out in one atlas);
    // Instances: every instance's prototype mesh where it sits (scales and bent ones as Scales).
    enum class Make { Auto, Strands, Scales, Feathers, Cards, Instances };
    Make make = Make::Auto;
    enum class Style { Tube, Ribbon };
    Style style = Style::Tube;
    int sides = 4;            // a tube's sides (3 or more)
    float widthScale = 1.0f;  // times each point's width (a diameter)
    float minWidth = 0.0f;    // no ring narrower than this (0: as the widths say)
    bool strandUVs = false;   // also lay every strand out in its own cell of a grid (uv2)
};

// The strands as one mesh (RachisMesh's layout, buildRachisMesh's geometry): per strand a ring
// at every point, `sides` vertices round a tube (a fan closing the tip) or two across a ribbon,
// the ring's width the point's width times widthScale. A ribbon lies in the plane of the
// strand and its side vector: the scalp's normal at the root (when the strand is rooted and a
// scalp is given, else any direction across the root), carried along by rotation-minimizing
// frames, so ribbons lie flat along the scalp. UVs: u across (0..1 round a tube, a seam
// column of its own), v root (0) to tip (1) by arc length. `featherIndex` holds each vertex's
// strand (its index in `strands`), `faceFeather` each face's. With strandUVs, u2/v2 (parallel
// to mesh.u/v, the same faceUVs) put strand k in cell k of a ceil(sqrt(n)) square grid.
// Strands with fewer than two points are skipped.
// Each face's part of the mesh (StrandMesh::faceParts): a host's face sets or groups.
enum class MeshPart : int { Strands = 0, Scales = 1, Rachis = 2, Cards = 3, Instances = 4 };
struct StrandMesh {
    RachisMesh mesh;
    std::vector<float> u2, v2;
    std::vector<int> faceParts;  // per face: its MeshPart
};
// What `options.make` makes of these strands (Auto resolved; never Auto).
StrandMeshOptions::Make resolveMeshMake(const StrandSet& strands, const StrandMeshOptions& options);
// The mesh `options.make` asks for. Scales and instances: their prototypes' meshes (the built-in
// ones, else `prototypes`, the host's scene meshes; a scale without one is a quad, an instance
// without one is left out), quads where a prototype's triangles pair into them (the built-in
// plates are quads but for a tip), featherIndex and faceFeather the instance's index. Feathers:
// the barbs' tubes, then the rachis tubes. Cards: the strands' card layers (built when the
// evaluation built cards), laid out in one atlas.
StrandMesh buildStrandMesh(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options,
                           const PrototypeLookup& prototypes = {});

// buildStrandMesh's points alone, for a frame where only the strands' points moved (the same
// strands, the same point counts: the topology and UVs stay): the very values buildStrandMesh
// gives mesh.points, in its order, the strands in parallel. `out` holds
// strandMeshPointCount(strands, options) of them. (For scales and feathers these build the whole
// mesh: their key changes with every new strand set.)
std::size_t strandMeshPointCount(const StrandSet& strands, const StrandMeshOptions& options,
                                 const PrototypeLookup& prototypes = {});
void strandMeshPoints(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options, Vec3f* out,
                      const PrototypeLookup& prototypes = {});
// What makes the mesh's topology and UVs: the style and sides, strandUVs, each strand's ID and point
// count. Equal keys: strandMeshPoints of the new strands fits the old mesh.
std::uint64_t strandMeshTopologyKey(const StrandSet& strands, const StrandMeshOptions& options);

// A StrandMesh node's settings (its params, strand_mesh_op.cpp): the mesh options, the preview
// density its strands are evaluated at (0..1) and at most how many of them make the mesh (0: all).
struct StrandMeshSettings {
    StrandMeshOptions options;
    float density = 1.0f;
    std::size_t maxStrands = 0;
};
StrandMeshSettings strandMeshSettings(const ParamSet& params);

// A StrandMesh node's strands as a session evaluated them (GroomSession::evaluateStrandMeshes) and
// the options its mesh is made with.
struct StrandMeshSource {
    std::string node;
    StrandsPtr strands;
    StrandMeshOptions options;
};
// Whether two lists of sources make the same mesh (the same strands, by pointer, and options).
bool sameStrandMeshes(const std::vector<StrandMeshSource>& a, const std::vector<StrandMeshSource>& b);

}  // namespace groomlab
