// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

// Static kd-tree over points for nearest and k-nearest queries (guide and
// clump-center lookups from strand roots). Build once, query from many
// threads.
//
// Layout: implicit and median-indexed. The tree over positions [begin, end)
// has its node at mid = begin + (end - begin) / 2, its left subtree over
// [begin, mid) and its right over [mid + 1, end); the points are stored in
// that order (contiguous, so a query walks memory in tree order), each with
// its index in the input. Large sets build their subtrees in parallel; the
// splits are the same as a serial build's, so every query's results (and
// the order of equally distant ones) are too.
class PointIndex {
public:
    PointIndex() = default;
    explicit PointIndex(std::vector<Vec3f> points);

    std::size_t size() const { return points_.size(); }
    bool empty() const { return points_.empty(); }

    // Index of the nearest point, or SIZE_MAX when empty.
    std::size_t nearest(const Vec3f& p, float* distance2 = nullptr) const;

    // The nearest point if it is within the squared distance `within2` (at it counts), else
    // SIZE_MAX: the index and distance nearest() gives then, found without searching past the
    // reach (a query far from every point looks at few of them).
    std::size_t nearestWithin(const Vec3f& p, float within2, float* distance2 = nullptr) const;

    // Up to k nearest points, closest first. Writes indices and squared
    // distances (both must hold k entries); returns how many were found.
    std::size_t kNearest(const Vec3f& p, std::size_t k, std::size_t* indices, float* distances2) const;

private:
    struct Item;  // a point and its index in the input, as the build moves them
    void build(Item* items, std::size_t begin, std::size_t end);

    std::vector<Vec3f> points_;         // in tree order: node i's point at i
    std::vector<std::uint32_t> ids_;    // each one's index in the input
    std::vector<std::uint8_t> axes_;    // each node's split axis
};

}  // namespace groomlab
