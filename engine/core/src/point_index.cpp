// SPDX-License-Identifier: Apache-2.0
#include "groomlab/point_index.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "groomlab/parallel.h"

namespace groomlab {

namespace {

float coord(const Vec3f& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

constexpr int kMaxDepth = 64;
// Subtrees of at least this many points build side by side (each is the same as built alone).
constexpr std::size_t kParallelBuild = 2048;

}  // namespace

// The points move with their indices (16 bytes together): the build reads each range's points in
// a row, not each through its index into the input.
struct PointIndex::Item {
    Vec3f p;
    std::uint32_t id;
};

PointIndex::PointIndex(std::vector<Vec3f> points) {
    const std::size_t n = points.size();
    std::vector<Item> items(n);
    parallelFor(0, n, [&](std::size_t i) { items[i] = {points[i], static_cast<std::uint32_t>(i)}; }, 4096);
    axes_.resize(n);
    build(items.data(), 0, n);
    // Points in tree order: a node's point sits at its own position.
    points_.resize(n);
    ids_.resize(n);
    parallelFor(0, n, [&](std::size_t i) {
        points_[i] = items[i].p;
        ids_[i] = items[i].id;
    }, 4096);
}

void PointIndex::build(Item* items, std::size_t begin, std::size_t end) {
    if (begin >= end) return;

    // Split on the axis with the largest extent (robust for flat point sets).
    Vec3f lo = items[begin].p, hi = lo;
    for (std::size_t i = begin + 1; i < end; ++i) {
        const Vec3f& p = items[i].p;
        lo = minPerAxis(lo, p);
        hi = maxPerAxis(hi, p);
    }
    const Vec3f ext = hi - lo;
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;

    // Equal coordinates in index order: a total order, so the median and each half's points are
    // the same whatever the standard library's nth_element leaves them in, and so is the tree
    // (which of several equally near points a search meets first, and returns): every position
    // ends up holding the same point however each range was shuffled on the way.
    const std::size_t mid = begin + (end - begin) / 2;
    auto split = [&](auto key) {
        std::nth_element(items + begin, items + mid, items + end, [&](const Item& a, const Item& b) {
            const float ca = key(a.p), cb = key(b.p);
            return ca < cb || (ca == cb && a.id < b.id);
        });
    };
    if (axis == 0) {
        split([](const Vec3f& p) { return p.x; });
    } else if (axis == 1) {
        split([](const Vec3f& p) { return p.y; });
    } else {
        split([](const Vec3f& p) { return p.z; });
    }
    axes_[mid] = static_cast<std::uint8_t>(axis);

    // The two halves never touch each other's part of `items`.
    if (end - begin >= kParallelBuild) {
        parallelInvoke([&] { build(items, begin, mid); }, [&] { build(items, mid + 1, end); });
    } else {
        build(items, begin, mid);
        build(items, mid + 1, end);
    }
}
std::size_t PointIndex::nearest(const Vec3f& p, float* distance2) const {
    std::size_t index = SIZE_MAX;
    float d2 = std::numeric_limits<float>::max();
    if (kNearest(p, 1, &index, &d2) == 0) return SIZE_MAX;
    if (distance2) *distance2 = d2;
    return index;
}

std::size_t PointIndex::nearestWithin(const Vec3f& p, float within2, float* distance2) const {
    // A reach that bounds nothing (or is no number): the plain search, then the test.
    if (!(within2 < std::numeric_limits<float>::max())) {
        float d2 = 0.0f;
        const std::size_t index = nearest(p, &d2);
        if (index == SIZE_MAX || !(d2 <= within2)) return SIZE_MAX;
        if (distance2) *distance2 = d2;
        return index;
    }
    if (points_.empty() || within2 < 0.0f) return SIZE_MAX;
    // nearest()'s search, started with the reach in place of the best found so far (just past it:
    // a point at the reach counts). A subtree it skips holds no point nearer than its plane, so
    // none nearer than the bound: the point found is the one nearest() meets first.
    float best = std::nextafter(within2, std::numeric_limits<float>::max());
    std::size_t index = SIZE_MAX;
    struct Entry {
        std::uint32_t begin, end;
        float planeDistance2;
    };
    Entry stack[2 * kMaxDepth + 2];
    int top = 0;
    stack[top++] = {0, static_cast<std::uint32_t>(points_.size()), 0.0f};
    while (top > 0) {
        const Entry e = stack[--top];
        if (e.planeDistance2 >= best) continue;
        const std::uint32_t mid = e.begin + (e.end - e.begin) / 2;
        const Vec3f d = points_[mid] - p;
        const float d2 = d.dot(d);
        if (d2 < best) {
            best = d2;
            index = ids_[mid];
        }
        const int axis = axes_[mid];
        const float diff = coord(p, axis) - coord(points_[mid], axis);
        const Entry left{e.begin, mid, 0.0f}, right{mid + 1, e.end, 0.0f};
        Entry nearChild = diff < 0 ? left : right;
        Entry farChild = diff < 0 ? right : left;
        farChild.planeDistance2 = diff * diff;
        if (farChild.begin < farChild.end && top < 2 * kMaxDepth + 1) stack[top++] = farChild;
        if (nearChild.begin < nearChild.end && top < 2 * kMaxDepth + 1) stack[top++] = nearChild;
    }
    if (index != SIZE_MAX && distance2) *distance2 = best;
    return index;
}

std::size_t PointIndex::kNearest(const Vec3f& p, std::size_t k, std::size_t* indices, float* distances2) const {
    if (points_.empty() || k == 0) return 0;
    std::size_t found = 0;

    // Sorted insertion into the result arrays (k is small).
    auto consider = [&](std::size_t node) {
        const Vec3f d = points_[node] - p;
        const float d2 = d.dot(d);
        if (found == k && d2 >= distances2[k - 1]) return;
        std::size_t pos = std::min(found, k - 1);
        while (pos > 0 && distances2[pos - 1] > d2) {
            distances2[pos] = distances2[pos - 1];
            indices[pos] = indices[pos - 1];
            --pos;
        }
        distances2[pos] = d2;
        indices[pos] = ids_[node];
        if (found < k) ++found;
    };

    // Depth-first with a small explicit stack; each entry is a subtree (its
    // range) and the squared distance to its parent's splitting plane, for pruning.
    struct Entry {
        std::uint32_t begin, end;
        float planeDistance2;
    };
    Entry stack[2 * kMaxDepth + 2];
    int top = 0;
    stack[top++] = {0, static_cast<std::uint32_t>(points_.size()), 0.0f};
    while (top > 0) {
        const Entry e = stack[--top];
        if (found == k && e.planeDistance2 >= distances2[k - 1]) continue;
        const std::uint32_t mid = e.begin + (e.end - e.begin) / 2;
        consider(mid);

        const int axis = axes_[mid];
        const float diff = coord(p, axis) - coord(points_[mid], axis);
        // Children: left [begin, mid), right [mid + 1, end); empty ones are skipped.
        const Entry left{e.begin, mid, 0.0f}, right{mid + 1, e.end, 0.0f};
        Entry nearChild = diff < 0 ? left : right;
        Entry farChild = diff < 0 ? right : left;
        farChild.planeDistance2 = diff * diff;
        if (farChild.begin < farChild.end && top < 2 * kMaxDepth + 1) stack[top++] = farChild;
        if (nearChild.begin < nearChild.end && top < 2 * kMaxDepth + 1) stack[top++] = nearChild;
    }
    return found;
}

}  // namespace groomlab
