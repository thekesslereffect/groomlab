// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scales (dragons, reptiles, pangolins): rigid plates, not hair. The strands lay them out (Scatter,
// Flow, Bend and maps decide where each sits, which way it points and its size: its strand's
// length is the scale's length, its root width the scale's breadth, its HairColor its color) and
// the Scales node puts one at each strand's root (InstanceSet::Instance::scale,
// placed flat on the skin by InstanceSet::resolve). This makes their geometry for every host: each
// scale's prototype (a scene mesh, or one of the built-in plates below) tilted up along the flow
// so its tip rests over the next scale's base (lift, tile), raised a little of its own against
// z-fighting (stack), then bent onto the skin (conform: each vertex laid onto the surface below
// it, along the normal, at its height above the skin plus the offset, never inside it), with
// per-scale data for shaders (color, a random ID, the root's uv, the variant). The viewport draws
// the mesh (DisplayBuilder's scales item), Arnold renders it as one polymesh with that data as
// user data (arnold_curves.h setScaleMesh), and exports to engines read it from here.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "groomlab/instances.h"
#include "groomlab/math.h"

namespace groomlab {

class StrandSet;
class ScalpMesh;

// ---- Built-in plates --------------------------------------------------------------------------

// A scale plate made in the core: its base at the origin, +Y its length (0 to 1), X across
// (`width` wide at its widest), +Z off the skin (0 underneath, `height` at the dome's top). One
// closed piece of quads for subdivision, the top and the underside alike: from a loop round the
// rim's middle (`rim` of the height is the rim's top), a crease loop at each edge, a support loop
// holding it, then an inner loop filled with a columns x rows grid of quads; a domed top with a keel
// down the middle, a flat underside; uvs planar (u across, v along).
struct ScalePlateShape {
    int rows = 4;          // quads along the middle grid (and each long side's segments)
    int columns = 4;       // quads across it (even: made so)
    float width = 1.0f;    // at the widest, over the length (1: as wide as the strand)
    float base = 0.55f;    // the base's width over the widest
    float widest = 0.35f;  // where it is widest, 0 base to 1 tip
    float tip = 1.0f;      // 1 pointed, 0 rounded
    float height = 0.1f;   // the dome's top over the length
    float rim = 0.35f;     // the rim's height over the dome's
    float keel = 0.04f;    // the ridge down the middle, over the length
};
InstancePrototype scalePlate(const ScalePlateShape& shape);

// The built-in prototypes, by the name an Instance node's prototype gives: "groomlab:scale" (keeled
// and pointed, slightly domed and bevelled: the default), "groomlab:scaleRound" (a rounded tip, no
// keel) and "groomlab:scaleShield" (broad and blunt, flatter). Null for any other name (a mesh of
// the host's scene).
const InstancePrototype* builtinPrototype(const std::string& name);
std::vector<std::string> builtinPrototypeNames();

// The prototypes a graph's Instance nodes name (its JSON, compounds' inner graphs too) that are
// not built-in plates: the scene meshes a groom file stores (GroomFile::prototypes). Sorted, once
// each.
std::vector<std::string> graphMeshPrototypes(const std::string& graphJson);

// The meshes an Instance node's prototype param names (instancePrototypeNames) that are neither
// built-in plates nor had by the host (`has`: its scene's meshes, a groom file's prototypes): what
// the node's note names, a node that draws and renders nothing. In order, once each.
std::vector<std::string> missingInstancePrototypes(const std::string& prototypeParam,
                                                   const std::function<bool(const std::string&)>& has);

// ---- The scales' mesh -------------------------------------------------------------------------

struct ScaleMesh {
    // Per vertex: positions, normals, tangents (along the scale), the prototype's uvs (zeros
    // without), the strand's color at the vertex's place along the scale (HairColor's: base to tip).
    std::vector<Vec3f> points, normals, tangents;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 3>> colors;
    std::vector<std::uint32_t> triangles;  // three vertex indices each
    struct Scale {
        std::uint64_t id = 0, strand = 0;
        std::size_t index = 0;          // its instance's place in the InstanceSet
        std::uint32_t prototype = 0;    // the InstanceSet's prototype index
        std::uint16_t variant = 0;      // its index among its node's prototypes
        std::int32_t material = -1;     // the InstanceSet's material index (-1: the prototype's own)
        std::uint32_t firstVertex = 0, vertexCount = 0, firstTriangle = 0, triangleCount = 0;
        float random = 0.0f;            // 0-1, from the ID: a shader's per-scale variation
        std::array<float, 2> rootUv{};  // the scalp's uv at its strand's root
        std::array<float, 3> color{};   // its strand's color at the root
        // Its frame, tilted (lift, tile) and raised (offset, stack), before the bend: rows of a
        // row-major matrix as InstanceSet::Placed has them (what an engine's instance takes).
        Vec3f x, y, z, origin;
        bool quad = false;  // drawn as a flat quad (the viewport's preview)
        void matrix(float out[16]) const;
    };
    std::vector<Scale> scales;
    std::size_t vertexCount() const { return points.size(); }
    std::size_t triangleCount() const { return triangles.size() / 3; }
};

struct ScaleMeshOptions {
    // The viewport: scales whose preview is auto draw as flat quads when their meshes together
    // would take more triangles than this (0: no limit); quads always for preview quads. A render
    // (preview off) takes meshes for every scale.
    bool preview = false;
    std::size_t triangleBudget = 0;
};

// A host's prototype by name (its scene's meshes; null: none). The built-in ones need none.
using PrototypeLookup = std::function<const InstancePrototype*(const std::string&)>;

// Builds the scales' mesh and keeps each scale's bent vertices: a scale whose frame, prototype,
// offset and skin did not change since the last build is copied, not bent again (a groom tweak
// that moved some strands bends only theirs). Parallel inside, the same output as serial. One
// thread at a time.
class ScaleMeshBuilder {
public:
    // The scales among strands.instances() whose strand is in `strands`, on `scalp` (the mesh the
    // strands are rooted on: their normals and the bend; null: flat, the normals the node saw).
    // A prototype that cannot be found draws as a quad. Null when there are no scales.
    std::shared_ptr<const ScaleMesh> build(const StrandSet& strands, const std::shared_ptr<const ScalpMesh>& scalp,
                                           const PrototypeLookup& prototypes = {}, const ScaleMeshOptions& options = {});
    struct Stats {
        std::size_t scales = 0, bent = 0, kept = 0, quads = 0;
    };
    const Stats& stats() const { return stats_; }
    void clear();

private:
    struct Kept {
        std::uint64_t key = 0;
        std::uint32_t firstVertex = 0, vertexCount = 0;
    };
    std::shared_ptr<const ScaleMesh> last_;
    std::unordered_map<std::uint64_t, Kept> kept_;  // by scale ID
    Stats stats_;
};

// The same without keeping anything.
std::shared_ptr<const ScaleMesh> buildScaleMesh(const StrandSet& strands, const std::shared_ptr<const ScalpMesh>& scalp,
                                                const PrototypeLookup& prototypes = {}, const ScaleMeshOptions& options = {});

}  // namespace groomlab
