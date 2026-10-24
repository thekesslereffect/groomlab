// SPDX-License-Identifier: Apache-2.0
// Cards pushed apart (separateCards, cards.h): a card closer than its gap to another, or passing
// through it, moves along the other's normal until the gap is kept, held at its root. A gap is a
// share of a card's width (the narrower card's of a pair), so it fits a groom at any scale and a
// small covert feather keeps a smaller one than a flight feather. Each pass
// every card takes the mean of what its neighbours ask of each of its stations (its own share:
// half of each correction), so the passes run side by side and come out the same every time.
//
// A card's shape here is its reference shape (the fixed resolution its texture is made from) plus
// its shift: the stations, and so the mesh, carry the shift; the texture stays where the strands
// are. A feather card's halves fold out of its plane (Card::spine): a point beside it is measured
// against the half it lies over.

#include <algorithm>
#include <cmath>
#include <vector>

#include "cards_internal.h"
#include "groomlab/cards.h"
#include "groomlab/parallel.h"

namespace groomlab {

namespace {

// Where the push may move a card, root (0) to tip (1): none at the root, all of it from a third
// of the way along (smoothly).
float freedom(float u) {
    const float t = std::clamp(u / 0.33f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Box {
    Vec3f lo, hi;
    bool overlaps(const Box& o) const {
        return lo.x <= o.hi.x && o.lo.x <= hi.x && lo.y <= o.hi.y && o.lo.y <= hi.y && lo.z <= o.hi.z && o.lo.z <= hi.z;
    }
    bool contains(const Vec3f& p) const {
        return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
    }
};

// A card's shape for the solver: per reference station its center (shifted), side, normal, half
// width, and its folds (a spine card's).
struct Shape {
    std::vector<Vec3f> center, side, normal;
    std::vector<float> half, foldRight, foldLeft;
    float spineOffset = 0.0f;
    bool spine = false;
    Box box;
};

Shape shapeOf(const CardMesh::Card& card, float gap) {
    Shape s;
    const std::size_t n = card.refCenter.size();
    s.center.resize(n);
    for (std::size_t i = 0; i < n; ++i) s.center[i] = card.refCenter[i] + (card.refShift.size() == n ? card.refShift[i] : Vec3f{});
    s.side = card.refSide;
    s.normal = card.refNormal;
    s.half.resize(n);
    for (std::size_t i = 0; i < n; ++i) s.half[i] = 0.5f * card.refWidth[i];
    s.spine = card.spine && card.across == 2 && card.refFoldRight.size() == n && card.refFoldLeft.size() == n;
    if (s.spine) {
        s.foldRight = card.refFoldRight;
        s.foldLeft = card.refFoldLeft;
        s.spineOffset = card.spineOffset;
    }
    Vec3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (std::size_t i = 0; i < n; ++i) {
        const float lift = s.spine ? s.half[i] * 2.0f * std::max(std::abs(s.foldRight[i]), std::abs(s.foldLeft[i])) : 0.0f;
        const float r = s.half[i] + lift + gap;
        lo = {std::min(lo.x, s.center[i].x - r), std::min(lo.y, s.center[i].y - r), std::min(lo.z, s.center[i].z - r)};
        hi = {std::max(hi.x, s.center[i].x + r), std::max(hi.y, s.center[i].y + r), std::max(hi.z, s.center[i].z + r)};
    }
    s.box = {lo, hi};
    return s;
}

// The points of a station a neighbour is tested with: the middle and near each edge (a spine
// card's edges lifted by their folds).
void stationPoints(const Shape& s, std::size_t i, Vec3f out[3]) {
    const float h = s.half[i] * 0.9f;
    if (s.spine) {
        const float at = std::clamp(s.spineOffset, -s.half[i], s.half[i]);
        out[0] = s.center[i] - s.side[i] * h + s.normal[i] * ((h + at) * s.foldLeft[i]);
        out[1] = s.center[i] + s.side[i] * at;
        out[2] = s.center[i] + s.side[i] * h + s.normal[i] * ((h - at) * s.foldRight[i]);
        return;
    }
    out[0] = s.center[i] - s.side[i] * h;
    out[1] = s.center[i];
    out[2] = s.center[i] + s.side[i] * h;
}

// Where point q lies over card s: its distance off the card's surface (along the surface's
// normal there) and that normal, the reference station before it and how far to the next; false
// when q is past the card's edges or ends.
struct Over {
    float distance = 0.0f;
    Vec3f normal;
    std::size_t station = 0;
    float t = 0.0f;
};

bool over(const Shape& s, const Vec3f& q, Over* out) {
    const std::size_t n = s.center.size();
    // The segments either side of the station nearest q (a card bends gently between stations).
    std::size_t near = 0;
    float nearest = 1e30f;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f d = q - s.center[i];
        const float d2 = d.dot(d);
        if (d2 < nearest) nearest = d2, near = i;
    }
    float best = 1e30f;
    bool found = false;
    for (std::size_t i = near > 0 ? near - 1 : 0; i <= near && i + 1 < n; ++i) {
        const Vec3f a = s.center[i], ab = s.center[i + 1] - a;
        const float len2 = ab.dot(ab);
        if (len2 <= 1e-20f) continue;
        const float t = (q - a).dot(ab) / len2;
        if (t < 0.0f || t > 1.0f) continue;
        const Vec3f c = a + ab * t;
        const Vec3f side = (s.side[i] * (1.0f - t) + s.side[i + 1] * t).normalized();
        Vec3f normal = (s.normal[i] * (1.0f - t) + s.normal[i + 1] * t).normalized();
        const float half = s.half[i] * (1.0f - t) + s.half[i + 1] * t;
        float across = (q - c).dot(side);
        Vec3f origin = c;
        if (s.spine) {
            // Over a folded half: its own plane, through the spine, tilted by the fold.
            const float at = std::clamp(s.spineOffset, -half, half);
            origin = c + side * at;
            across = (q - origin).dot(side);
            const float fold = across >= 0.0f ? s.foldRight[i] * (1.0f - t) + s.foldRight[i + 1] * t
                                              : -(s.foldLeft[i] * (1.0f - t) + s.foldLeft[i + 1] * t);
            normal = (normal - side * fold).normalized();
            if (across > half - at || -across > half + at) continue;
        } else if (std::abs(across) > half) {
            continue;
        }
        const float distance = (q - origin).dot(normal);
        const float off = (q - (origin + side * across)).length();  // how far from the card's middle line, all told
        if (off < best) {
            best = off;
            *out = {distance, normal, i, t};
            found = true;
        }
    }
    return found;
}

}  // namespace

void separateCards(std::vector<CardMesh::Card>& cards, float hairGap, float featherGap, int iterations) {
    const std::size_t count = cards.size();
    if (count < 2 || iterations <= 0) return;
    // A card's share (its kind's), its width (the widest station) and the gap it keeps (the
    // share of its width: the most any pair with it asks).
    auto shareOf = [&](std::size_t c) { return cards[c].feather >= 0 ? featherGap : hairGap; };
    std::vector<float> widthOf(count, 0.0f);
    for (std::size_t c = 0; c < count; ++c) {
        for (const float w : cards[c].refWidth) widthOf[c] = std::max(widthOf[c], w);
    }
    auto gapOf = [&](std::size_t c) { return shareOf(c) * widthOf[c]; };
    auto pairGap = [&](std::size_t a, std::size_t b) { return std::max(shareOf(a), shareOf(b)) * std::min(widthOf[a], widthOf[b]); };
    // How far off a card's surface a neighbour's point still counts as meeting it (half the
    // narrower card's width): a card through another is never deeper than that where they cross,
    // and points farther off belong to cards that only pass by (their boxes meet, they don't).
    auto reachOf = [&](std::size_t a, std::size_t b) { return 0.5f * std::min(widthOf[a], widthOf[b]); };
    for (CardMesh::Card& card : cards) {
        if (card.refCenter.size() >= 2 && card.refShift.size() != card.refCenter.size()) card.refShift.assign(card.refCenter.size(), Vec3f{});
    }
    for (int pass = 0; pass < iterations; ++pass) {
        std::vector<Shape> shapes(count);
        parallelFor(0, count, [&](std::size_t c) {
            if (cards[c].refCenter.size() >= 2) shapes[c] = shapeOf(cards[c], gapOf(c));
        }, 16);
        // The pairs whose boxes meet (sorted by their low x: the sweep), each card's partners in order.
        std::vector<std::size_t> order;
        for (std::size_t c = 0; c < count; ++c) {
            if (!shapes[c].center.empty()) order.push_back(c);
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return shapes[a].box.lo.x < shapes[b].box.lo.x || (shapes[a].box.lo.x == shapes[b].box.lo.x && a < b);
        });
        // Each card's partners: (the other card, the pair's index), in order of the other card.
        struct Pair {
            std::size_t a, b;   // a < b
            float side = 1.0f;  // which side of a card b keeps (+1 along a's normal)
        };
        std::vector<Pair> pairs;
        std::vector<std::vector<std::pair<std::size_t, std::size_t>>> partners(count);
        for (std::size_t i = 0; i < order.size(); ++i) {
            const std::size_t a = order[i];
            for (std::size_t j = i + 1; j < order.size() && shapes[order[j]].box.lo.x <= shapes[a].box.hi.x; ++j) {
                const std::size_t b = order[j];
                const bool sameFeather = cards[a].feather >= 0 && cards[a].feather == cards[b].feather;
                if (sameFeather || pairGap(a, b) <= 0.0f || !shapes[a].box.overlaps(shapes[b].box)) continue;
                partners[a].push_back({b, pairs.size()});
                partners[b].push_back({a, pairs.size()});
                pairs.push_back({std::min(a, b), std::max(a, b)});
            }
        }
        for (auto& p : partners) std::sort(p.begin(), p.end());
        // Each pair's side: the side of a most of b's points within reach lie on (a card through
        // another goes to that side), b above when they are even. Once per pair and pass.
        parallelFor(0, pairs.size(), [&](std::size_t k) {
            const std::size_t a = pairs[k].a, b = pairs[k].b;
            const Shape& sa = shapes[a];
            const Shape& sb = shapes[b];
            const float reach = reachOf(a, b);
            float sum = 0.0f;
            for (std::size_t i = 0; i < sb.center.size(); ++i) {
                Vec3f pts[3];
                stationPoints(sb, i, pts);
                for (const Vec3f& q : pts) {
                    Over o;
                    if (sa.box.contains(q) && over(sa, q, &o) && std::abs(o.distance) < reach) sum += o.distance;
                }
            }
            pairs[k].side = sum >= 0.0f ? 1.0f : -1.0f;
        }, 16);
        // Each card's own moves: from its points over a neighbour (pushed off it), and from the
        // neighbour's points over it (pushed the other way), half of each correction. A point
        // outside the other card's box (grown by its gap) can't be within reach of it.
        std::vector<std::vector<Vec3f>> moves(count);
        std::vector<float> largest(count, 0.0f);
        parallelFor(0, count, [&](std::size_t c) {
            const Shape& sc = shapes[c];
            const std::size_t n = sc.center.size();
            if (n == 0 || partners[c].empty()) return;
            std::vector<Vec3f> sum(n, Vec3f{});
            std::vector<float> weight(n, 0.0f);
            for (const auto& [p, k] : partners[c]) {
                const float gap = pairGap(c, p), reach = reachOf(c, p);
                // The side c keeps of p (the pair's side is b's of a).
                const float side = c == pairs[k].b ? pairs[k].side : -pairs[k].side;
                const Shape& sp = shapes[p];
                for (std::size_t i = 0; i < n; ++i) {  // c's points over p
                    Vec3f pts[3];
                    stationPoints(sc, i, pts);
                    for (const Vec3f& q : pts) {
                        Over o;
                        if (!sp.box.contains(q) || !over(sp, q, &o) || std::abs(o.distance) >= reach) continue;
                        const float need = gap - side * o.distance;
                        if (need <= 0.0f) continue;
                        sum[i] += o.normal * (side * 0.5f * need);
                        weight[i] += 1.0f;
                    }
                }
                for (std::size_t i = 0; i < sp.center.size(); ++i) {  // p's points over c
                    Vec3f pts[3];
                    stationPoints(sp, i, pts);
                    for (const Vec3f& q : pts) {
                        Over o;
                        if (!sc.box.contains(q) || !over(sc, q, &o) || std::abs(o.distance) >= reach) continue;
                        const float need = gap + side * o.distance;  // p keeps the side opposite c's
                        if (need <= 0.0f) continue;
                        const Vec3f push = o.normal * (side * 0.5f * need);
                        sum[o.station] += push * (1.0f - o.t);
                        weight[o.station] += 1.0f - o.t;
                        sum[o.station + 1] += push * o.t;
                        weight[o.station + 1] += o.t;
                    }
                }
            }
            std::vector<Vec3f> move(n, Vec3f{});
            for (std::size_t i = 0; i < n; ++i) {
                if (weight[i] <= 0.0f) continue;
                move[i] = sum[i] * (freedom(static_cast<float>(i) / static_cast<float>(n - 1)) / weight[i]);
                largest[c] = std::max(largest[c], move[i].length());
            }
            moves[c] = std::move(move);
        }, 4);
        // Settled (every card moved less than a twentieth of its gap this pass): done.
        bool moved = false;
        for (std::size_t c = 0; c < count; ++c) {
            std::vector<Vec3f>& shift = cards[c].refShift;
            const std::vector<Vec3f>& move = moves[c];
            if (move.empty()) continue;
            moved = moved || largest[c] > 0.05f * gapOf(c);
            for (std::size_t i = 0; i < shift.size(); ++i) shift[i] += move[i];
            // Smoothed along the card (1 2 1), the root held: no kink where a neighbour ends.
            const std::vector<Vec3f> was = shift;
            for (std::size_t i = 1; i + 1 < shift.size(); ++i) shift[i] = (was[i - 1] + was[i] * 2.0f + was[i + 1]) * 0.25f;
            shift[0] = Vec3f{};
        }
        if (!moved) break;
    }
    for (CardMesh::Card& card : cards) {
        if (card.refShift.empty()) continue;
        bool any = false;
        for (const Vec3f& v : card.refShift) any = any || v.dot(v) > 0.0f;
        if (!any) {
            card.refShift.clear();
            continue;
        }
        detail::sampleStations(card);
    }
}

}  // namespace groomlab
