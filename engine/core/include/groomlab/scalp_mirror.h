// SPDX-License-Identifier: Apache-2.0
#pragma once

// Topological symmetry: a scalp mirrored across its own surface instead of a world plane. Each
// vertex gets a partner, found once on the scalp's rest pose mirrored in x; a point anywhere is
// then carried to its partner spot on the scalp as it is now, whatever its pose (a head turned,
// tilted or deforming), and a brush, a placed guide or a painted dab lands on the other side.

#include <cstdint>
#include <memory>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

class ScalpMirror {
public:
    // The partners of `scalp`'s vertices, found on its rest pose (ScalpMesh::restPose: its rest
    // points, else its points) mirrored across x = the middle of its bounds in x: the vertex at
    // the mirrored spot, or (a mesh not quite symmetric) the nearest point of the surface there.
    explicit ScalpMirror(const ScalpMesh& scalp);
    // The mirror of a scalp, made once per rest pose and topology and kept (the same object for
    // every frame of a deforming scalp). Thread-safe.
    static std::shared_ptr<const ScalpMirror> of(const ScalpMesh& scalp);

    // Whether `scalp` has the topology this mirror was made for.
    bool fits(const ScalpMesh& scalp) const;
    // A point mirrored on `scalp` (this mirror's topology, any pose): the nearest point of the
    // surface carried to its partner spot, and the point's offset from it (its height above the
    // surface) carried through the mirrored frame. `p` itself when the scalp does not fit.
    Vec3f point(const ScalpMesh& scalp, const Vec3f& p) const;
    // A direction at point `at` mirrored the same way (a stroke's drag, a flow arrow).
    Vec3f vector(const ScalpMesh& scalp, const Vec3f& at, const Vec3f& v) const;
    // The side of the mirror a point is on (the side of its nearest surface point on the rest
    // pose, whatever the pose now): 1 past the middle in x, -1 before it, 0 on the seam (within
    // a ten-thousandth of the scalp's size); 0 when the scalp does not fit. `root`, when on the
    // scalp, is the point's own surface point (no search).
    int side(const ScalpMesh& scalp, const Vec3f& p, const StrandRoot* root = nullptr) const;

    // Each vertex's partner on the surface (face and barycentrics, ScalpMesh::position).
    const std::vector<StrandRoot>& partners() const { return partners_; }
    // Each vertex's partner vertex, or -1 where the partner is a point between vertices.
    const std::vector<int>& pairs() const { return pairs_; }
    // How many vertices found a vertex as their partner (the rest found the surface).
    std::size_t pairedCount() const;
    std::uint64_t key() const { return key_; }

private:
    struct Frame;
    bool frameAt(const ScalpMesh& scalp, const Vec3f& p, Frame& out) const;

    std::vector<StrandRoot> partners_;
    std::vector<int> pairs_;
    std::size_t points_ = 0, triangles_ = 0;
    std::uint64_t key_ = 0;
    float middle_ = 0.0f, seam_ = 0.0f;  // the rest pose's middle in x; how near it is on the seam
};

}  // namespace groomlab
