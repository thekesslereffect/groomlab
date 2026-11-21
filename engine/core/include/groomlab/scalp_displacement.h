// SPDX-License-Identifier: Apache-2.0
#pragma once

// Displacement on the scalp (issue #89): a creature's displacement map, as its renderer displaces
// the skin, so the hair roots on the surface the render shows. A Scalp node's displacement params
// name the map and how it reads; the session displaces its scalp with them after refining it
// (GroomSession: the Scalp node's subdivision, matching the render's), so everything rooted on or
// reading the scalp (Scatter, the guides, Interpolate, Collide) takes the displaced surface.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace groomlab {

class ScalpMesh;

// One scalp's displacement: the scalp it is for (a piece's name, scalp_pieces.h; the groom's own
// "scalp" covers the whole mesh when it has no pieces), the image (a path; "<UDIM>" for tiles;
// EXR, HDR, PNG, PNM, TGA, JPEG), and how a value v of it moves a point along its normal:
// (v - zero) * height, as Arnold's displacement (height its scale, zero its zero value) reads it.
struct ScalpDisplacement {
    std::string scalp;
    std::string path;
    float height = 1.0f;
    float zero = 0.0f;

    bool operator==(const ScalpDisplacement& o) const {
        return scalp == o.scalp && path == o.path && height == o.height && zero == o.zero;
    }
};

// `scalp` with its points (and its rest pose's) moved along their normals by each displacement's
// map, read at the point's UVs (the mean over its corners: a seam reads both sides; the red
// channel). Null when nothing moves (no displacement, a zero height, a mesh without UVs); a map
// that cannot be read leaves its scalp as it is and says why in *error.
std::shared_ptr<const ScalpMesh> displaceScalp(const std::shared_ptr<const ScalpMesh>& scalp,
                                               const std::vector<ScalpDisplacement>& displacements, std::string* error);

// What displacing with `displacements` reads: their settings and their images' file stamps (an
// image saved again displaces anew).
std::uint64_t displacementKey(const std::vector<ScalpDisplacement>& displacements);

}  // namespace groomlab
