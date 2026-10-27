// SPDX-License-Identifier: Apache-2.0
#pragma once

// Geometry instanced on strands (snow, dandruff, dew, burrs, beads): the
// Instance operator records where each instance sits on its strand, and
// hosts place them on the final strands with resolve(), so nodes after the
// Instance node (curl, collide, simulation) carry the instances along.
// Scales (the Scales node's) are instances too: one at each strand's
// root, lying on the skin; scales.h makes their meshes.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

class StrandSet;
class ScalpMesh;

// A prototype mesh instances take: object-space positions and normals per face-vertex (hard
// edges stay hard), its triangles and, when it has them, uvs per point. The host reads it from
// its scene; the built-in ones (scales.h: "groomlab:scale" and the rest) come from the core.
struct InstancePrototype {
    std::vector<Vec3f> points, normals;
    std::vector<std::uint32_t> triangles;   // three vertex indices each
    std::vector<std::array<float, 2>> uvs;  // per point, or empty
    std::uint64_t signature = 0;            // the host's fingerprint of the scene mesh (0: none)
    bool found = false;
};

// The prototype's +Y is its long axis: the axis each orientation aims, and the
// one a strand-fitted instance (fitStrand) stretches along the strand.
enum class InstanceOrient : std::uint8_t {
    Strand,  // the prototype's +Y follows the strand at that point
    Up,      // +Y points world up (settled snow)
    Root,    // +Y follows the strand's root direction
    Random,  // any direction (dandruff, debris)
};

class InstanceSet {
public:
    // Prototype names (a mesh in the host scene, or a built-in one); instances refer to them by index.
    std::vector<std::string> prototypes;
    // Materials by name (a shading group or shader in the host scene) scales render with;
    // instances refer to them by index (Instance::material).
    std::vector<std::string> materials;
    // The strands only lay the scales out (a Scales node, showStrands off):
    // hosts draw and render the scales, not the strands.
    bool hidesStrands = false;

    struct Instance {
        std::uint32_t prototype = 0;
        std::uint64_t strand = 0;  // StrandSet ID it sits on
        std::uint64_t id = 0;      // stable per instance
        float t = 1.0f;            // along the strand's length, 0 root to 1 tip
        InstanceOrient orient = InstanceOrient::Strand;
        Vec3f direction{0, 1, 0};  // Random: its +Y
        float spin = 0.0f;         // radians about +Y
        float scale = 1.0f;
        float offset = 0.0f;       // off the strand, along the instance's +X
        // Scale by the strand's width where it sits, over its root width (the
        // Width node's profile): thinner toward the tip, smaller instances.
        bool scaleByWidth = false;
        // Keep a fixed distance from the root, measured on the strand's rest pose,
        // instead of the fraction t of its length now: a strand that stretches
        // (simulated guides) doesn't slide the instance. The host fills
        // restDistance (t times the strand's rest length, pinRestDistances in
        // groom.h); until then (below 0) the instance sits at t like any other.
        bool pinToRest = false;
        float restDistance = -1.0f;
        // Fitted to its strand (the Instance node's sizeMode fitStrand): `scale`
        // is a fraction of the strand's rest length, the size along +Y; X and Z
        // are that times `aspect` (times the width factor with scaleByWidth), so
        // a leaf or feather spans its strand. The rest length (restLength, filled
        // by the host like restDistance) keeps the size still while the strand
        // stretches; until it is known (below 0) the length now is used.
        bool fitStrand = false;
        float aspect = 1.0f;
        float restLength = -1.0f;
        // A scale (the Scales node's): at its strand's root, +Y along the
        // strand's direction laid onto the skin, +Z the skin's normal (`direction` holds the
        // normal the node saw, for a host without the scalp). +Y is `scale` times the strand's
        // rest length (restLength, as a fitted one), X and Z `scale` times its root width (or
        // `aspect` of +Y without widths). `offset` is its height above the skin; scales.h
        // tilts it (lift, tile), stacks it and bends it onto the skin (conform).
        bool onSkin = false;
        bool conform = true;
        std::uint8_t preview = 0;    // the viewport: 0 auto, 1 meshes, 2 flat quads
        std::uint16_t variant = 0;   // which of the node's prototypes (its index in the node's list)
        std::int32_t material = -1;  // index into `materials` (-1: the prototype's own)
        float lift = 0.0f;           // radians the tip turns up, about +X at the base
        float tile = 0.0f;           // 0-1: the tip rises by this much of the plate's height
        float stack = 0.0f;          // a height of its own (a fraction of the plate's), against z-fighting
        // Which way +Y points on the skin: the strand's chord, root to tip (false), or its direction
        // near the root (aimRoot: the root to the point a tenth of its length along, at least its
        // first segment; a strand curling back still points the scale the way it leaves the skin).
        bool aimRoot = false;
        // Bent along the strand (the node's follow strand): the base on the skin, the rest along
        // the strand's curve over the plate's length (scales.h). Aims by the root.
        bool followStrand = false;
        // Bent along its strand (the Instance node's deform alongStrand): not placed by a matrix
        // but its mesh made like a scale's (scales.h, ScaleMeshBuilder), each vertex at its place
        // along the prototype's long axis (deformAxis: 0 x, 1 y, 2 z) laid onto the strand's arc
        // length from where the instance sits, across it in the strand's frame carried along
        // from there (no twist of its own). Its frame where it sits (resolve) faces +Z along the
        // feather's vane normal (Propagate's vaneNormalX/Y/Z) or the skin's normal at the root.
        bool deform = false;
        std::uint8_t deformAxis = 1;
        // Placed as a mesh of its own (a scale, or bent along its strand), not by a matrix.
        bool meshed() const { return onSkin || deform; }
    };
    std::vector<Instance> instances;

    std::size_t size() const { return instances.size(); }
    bool empty() const { return instances.empty(); }
    std::uint32_t prototypeIndex(const std::string& name);  // added when new
    void append(const InstanceSet& other);                  // prototypes merged by name
    std::uint64_t contentHash() const;
    // One instance into `h`, as contentHash takes each.
    static std::uint64_t hashInstance(std::uint64_t h, const Instance& i);
    // Whether any instance reads its strand's rest pose (pinned to its rest
    // distance, fitted to its rest length, a scale): the host evaluates the rest pose.
    bool needsRestPose() const;
    bool hasScales() const;
    // Whether any instance is a mesh of its own (Instance::meshed: a scale, or bent along its
    // strand): what ScaleMeshBuilder makes; the rest are placed by matrices.
    bool hasMeshes() const;
    std::uint32_t materialIndex(const std::string& name);  // added when new

    // An instance placed on the final strands: an origin and three axes
    // (scaled; X and Z shorter or longer than Y for a strand-fitted one, always
    // at right angles). Row-major Maya/Arnold matrix rows: x, y, z, origin.
    struct Placed {
        std::uint32_t prototype = 0;
        std::uint64_t id = 0;
        std::size_t index = 0;  // its place in `instances` (motion keys match instances by it)
        Vec3f x, y, z, origin;
        bool onSkin = false;  // a scale: Z along the skin's normal at its root
        bool meshed = false;  // its Instance::meshed(): ScaleMeshBuilder makes its mesh, no matrix places it
        float distance = 0.0f;  // where it sits along its strand, from the root (scene units)
        void matrix(float out[16]) const;
    };
    // Instances whose strand is in `strands` (preview density drops the rest),
    // in the order of `instances`. A pinned instance with a rest distance sits
    // that far along the strand (at most its length: past it, at the tip); a
    // fitted one with a rest length takes its size from it. Scales take the
    // skin's normal at their root from `scalp` (the mesh the strands are rooted
    // on), else the normal the node saw; they lie flat on it here (scales.h
    // tilts and bends them). One bent along its strand (deform) takes the strand's tangent for
    // +Y and its vane normal, else the skin's normal at the root (from `scalp`), for +Z.
    std::vector<Placed> resolve(const StrandSet& strands, const ScalpMesh* scalp = nullptr) const;
};

// An Instance node's prototype param as names: one, or several separated by spaces or commas
// (variants).
std::vector<std::string> instancePrototypeNames(const std::string& text);
}  // namespace groomlab
