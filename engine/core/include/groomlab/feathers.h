// SPDX-License-Identifier: Apache-2.0
#pragma once

// Feathers for game engines, the way Weta built the eagle of Unreal's "Meerkat" short: each
// feather's shaft (the rachis) is mesh geometry skinned into the character's skeletal mesh,
// and its barbs are groom strands rooted on that mesh, so Unreal's skinning binding carries
// them with it.
//
// A Propagate node with a shaftWidth above 0 records its shafts as rachis tubes (a
// FeatherLayer on its output strands, riding along to the groom's output like card layers),
// and roots every barb on the tube's surface. buildRachisMesh turns the layers into one mesh:
// the host shows it (groomLabShape's outRachis) and exports it for skinning.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

// The strand channel a Propagate node with a shaftWidth sets to 1 on each shaft it keeps (isShaft
// 1) and makes a rachis tube for: that shaft is as wide as its rachis, the renderers draw the tube
// in its place, and a Width node after leaves it (other shafts, a second Propagate's, 0).
constexpr const char* kRachisChannel = "rachis";

class StrandSet;

// Motion after a Propagate (Wind, Jiggle) is its barbs' own: they turn about their roots on the
// shaft, so the shafts (isShaft 1) hold where they are, as their rachis tubes do. A whole feather
// moves by motion on its shafts before Propagate, which grows the barbs along them.
// True when `strands` carry a shaft (isShaft above 0.5).
bool hasShafts(const StrandSet& strands);
// The shafts of `in` put back in `out` (the same strands, as a motion node makes them) as they came in.
void holdShafts(const StrandSet& in, StrandSet& out);

// A cross-section of a rachis tube at one of its shaft's points: the ring's center, its axes
// (side = tangent x normal: the right-hand vane; normal: the vane plane's normal), radius
// and arc position (0 root .. 1 tip). Vertex j of an n-sided ring sits at angle 2 pi j / n
// from `side` toward `normal`.
struct RachisRing {
    Vec3f center, tangent, normal, side;
    float radius = 0.0f;
    float u = 0.0f;
};

// What one Propagate node made that is not strands: its groom group (groupId, groupName: the
// group its strands export in; 0 none) and, with a shaftWidth, its shafts as rachis tubes.
struct FeatherLayer {
    std::string node;       // the Propagate node's path
    int groupId = 0;        // its strands' groupId (0: not set)
    std::string groupName;  // the group's name in engines ("" : none given)
    int sides = 6;          // sides of every tube; 2: a flat ribbon in the vane plane
    // Shaft k's rings are rings[offsets[k], offsets[k + 1]); featherIds[k] is its featherId.
    std::vector<std::size_t> offsets{0};
    std::vector<RachisRing> rings;
    std::vector<int> featherIds;

    bool hasRachis() const { return !rings.empty(); }
    std::size_t shaftCount() const { return featherIds.size(); }
};
using FeatherLayerPtr = std::shared_ptr<const FeatherLayer>;

// The point on a rachis's surface between rings `a` and `b` (the mesh's faces there), `f` of
// the way from a to b, where a ray from the center at angle `angle` (radians, from `side`
// toward `normal`, in a's frame) leaves the tube: on the polygon's edge (not the circle), so
// the point lies on the mesh. A ribbon (sides 2) takes the point across it at cos(angle).
Vec3f rachisSurfacePoint(const RachisRing& a, const RachisRing& b, float f, float angle, int sides);

// The rachis tubes of every layer as one polygon mesh. Per shaft: a ring of `sides` vertices
// at each shaft point, quads between rings (normals outward), a fan closing the tip (tubes;
// a ribbon is one strip). UVs: u around the tube (0..1, a seam column of its own), v root (0)
// to tip (1) by arc length. Every vertex carries its feather's index in the mesh
// (`featherIndex`: each layer's featherIds, a later layer's from groupNumberBase of the earlier
// ones' highest, as Merge numbers them) and group (`groupIds`); faces too.
struct RachisMesh {
    std::vector<Vec3f> points;
    std::vector<int> faceCounts;    // 3 or 4
    std::vector<int> faceVertices;  // faceCounts' worth per face
    std::vector<float> u, v;        // the UVs
    std::vector<int> faceUVs;       // per face-vertex: index into u, v
    std::vector<float> featherIndex;  // per point
    std::vector<float> groupIds;      // per point
    std::vector<int> faceFeather;     // per face: its feather's index
    std::size_t featherCount = 0;   // the tubes made (feathers with a rachis)

    std::size_t faceCount() const { return faceCounts.size(); }
    std::size_t triangleCount() const;
};

RachisMesh buildRachisMesh(const std::vector<FeatherLayerPtr>& layers);

// True when any layer has rachis tubes.
bool hasRachis(const std::vector<FeatherLayerPtr>& layers);

// groupId -> name, from the layers that name their group (the first one wins).
std::map<int, std::string> featherGroupNames(const std::vector<FeatherLayerPtr>& layers);

}  // namespace groomlab
