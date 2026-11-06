// SPDX-License-Identifier: Apache-2.0
#include "groomlab/cards.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>
#include <utility>

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

#include "cards_internal.h"

namespace groomlab {

// What the card files share (cards_internal.h).
using detail::kRefStations;
using detail::kFeatherIdChannel;
using detail::sampleStations;
using detail::seatCard;
using detail::CardShape;
using detail::CardPoint;
using detail::projectOntoCard;
using detail::cardFrames;
using detail::kMeans;
using detail::groupFeathers;
using detail::makeFeatherCards;

namespace {

// A spine card's half at a point `offset` across (Card::spine): its fold there (station i to
// i + 1 at w) and its frame, the flat card's side and normal turned by the fold: `across` runs
// in the half toward +side (left edge to spine to right edge), `normal` out of it.
struct SpineHalf {
    Vec3f across, normal;
    float fold = 0.0f;
};

SpineHalf spineHalf(const CardShape& card, float spineOffset, float offset, std::size_t i, float w, const Vec3f& side,
                    const Vec3f& normal) {
    SpineHalf h;
    const bool right = offset >= spineOffset;
    const std::vector<float>& folds = right ? card.foldRight : card.foldLeft;
    if (i + 1 < folds.size()) h.fold = folds[i] * (1.0f - w) + folds[i + 1] * w;
    // Right: the surface runs side + normal x fold out from the spine; left: -side + normal x fold.
    const float f = right ? h.fold : -h.fold;
    h.across = (side + normal * f).normalized();
    h.normal = (normal - side * f).normalized();
    return h;
}

}  // namespace

// ---- What the card files share (cards_internal.h) ----------------------------------------

namespace detail {

void sampleStations(CardMesh::Card& card) {
    const std::size_t n = static_cast<std::size_t>(std::max(card.stations, 2));
    const std::size_t last = card.refCenter.size() - 1;
    if (card.stationAlong.size() != n) {
        card.stationAlong.resize(n);
        for (std::size_t k = 0; k < n; ++k) card.stationAlong[k] = static_cast<float>(k) / static_cast<float>(n - 1);
    }
    card.center.clear(), card.side.clear(), card.normal.clear(), card.width.clear(), card.shading.clear();
    card.foldRight.clear(), card.foldLeft.clear();
    card.featherBarbShiftRight.clear(), card.featherBarbShiftLeft.clear();
    for (std::size_t k = 0; k < n; ++k) {
        const float f = std::clamp(card.stationAlong[k], 0.0f, 1.0f) * static_cast<float>(last);
        const std::size_t i = std::min(static_cast<std::size_t>(f), last - 1);
        const float w = f - static_cast<float>(i);
        Vec3f center = card.refCenter[i] * (1.0f - w) + card.refCenter[i + 1] * w;
        if (card.refShift.size() == card.refCenter.size()) center += card.refShift[i] * (1.0f - w) + card.refShift[i + 1] * w;
        card.center.push_back(center);
        card.side.push_back((card.refSide[i] * (1.0f - w) + card.refSide[i + 1] * w).normalized());
        card.normal.push_back((card.refNormal[i] * (1.0f - w) + card.refNormal[i + 1] * w).normalized());
        card.width.push_back(card.refWidth[i] * (1.0f - w) + card.refWidth[i + 1] * w);
        if (card.refFoldRight.size() == card.refCenter.size() && card.refFoldLeft.size() == card.refCenter.size()) {
            card.foldRight.push_back(card.refFoldRight[i] * (1.0f - w) + card.refFoldRight[i + 1] * w);
            card.foldLeft.push_back(card.refFoldLeft[i] * (1.0f - w) + card.refFoldLeft[i + 1] * w);
        }
        if (card.refFeatherBarbShiftRight.size() == card.refCenter.size()) {
            card.featherBarbShiftRight.push_back(card.refFeatherBarbShiftRight[i] * (1.0f - w) +
                                                 card.refFeatherBarbShiftRight[i + 1] * w);
        }
        if (card.refFeatherBarbShiftLeft.size() == card.refCenter.size()) {
            card.featherBarbShiftLeft.push_back(card.refFeatherBarbShiftLeft[i] * (1.0f - w) +
                                                card.refFeatherBarbShiftLeft[i + 1] * w);
        }
        if (card.refShading.size() == card.refCenter.size()) {
            card.shading.push_back((card.refShading[i] * (1.0f - w) + card.refShading[i + 1] * w).normalized());
        }
    }
}

std::vector<Vec3f> cardVertexRow(const CardMesh::Card& card, std::size_t i, const std::vector<Vec3f>& root) {
    std::vector<Vec3f> points = cardRow(card, i);
    const float along = i < card.stationAlong.size()
                            ? card.stationAlong[i]
                            : static_cast<float>(i) / static_cast<float>(std::max(card.stations - 1, 1));
    // Hair cards need at least two quads to ease from the scalp-seated root row into their
    // fitted plane. With the old fixed 10% blend, an eight-segment card's first interior row was
    // already outside the blend, so its first quad absorbed the entire root twist. Feather cards
    // retain their shorter, shaft-specific blend.
    const float segment = card.stationAlong.size() > 1 ? std::max(card.stationAlong[1] - card.stationAlong[0], 1e-6f)
                                                       : 1.0f / static_cast<float>(std::max(card.stations - 1, 1));
    const float baseBlend = card.feather >= 0 ? kBaseBlend : std::max(kBaseBlend, 3.0f * segment);
    if (card.base.size() == root.size() && along < baseBlend) {
        // A hair card's root row and first interior row move together. Letting the first row
        // receive only part of the scalp-seating offset shears the first quad and turns every
        // otherwise-straight baked strand into a small zigzag there. Above that rigid root quad,
        // ease the offset out smoothly. Feather seating keeps its shaft-specific falloff.
        const float held = card.feather >= 0 ? 0.0f : segment;
        const float t = std::clamp((along - held) / std::max(baseBlend - held, 1e-9f), 0.0f, 1.0f);
        const float w = 1.0f - t * t * (3.0f - 2.0f * t);
        for (std::size_t e = 0; e < points.size(); ++e) points[e] += (card.base[e] - root[e]) * w;
    }
    return points;
}

std::vector<Vec3f> cardRow(const CardMesh::Card& card, std::size_t i) {
    const float halfW = 0.5f * card.width[i];
    const Vec3f& side = card.side[i];
    const Vec3f& normal = card.normal[i];
    if (card.spine && card.across == 2) {
        const float at = std::clamp(card.spineOffset, -halfW, halfW);
        const float foldR = i < card.foldRight.size() ? card.foldRight[i] : 0.0f;
        const float foldL = i < card.foldLeft.size() ? card.foldLeft[i] : 0.0f;
        return {card.center[i] - side * halfW + normal * ((halfW + at) * foldL), card.center[i] + side * at,
                card.center[i] + side * halfW + normal * ((halfW - at) * foldR)};
    }
    const int across = std::max(card.across, 1);
    std::vector<Vec3f> row;
    row.reserve(static_cast<std::size_t>(across) + 1);
    if (card.tubeArc > 0.0f) {
        // A tube: around the center line from -tubeArc / 2 to tubeArc / 2, from the normal
        // toward the side (a closed tube's last vertex on its first).
        for (int e = 0; e <= across; ++e) {
            const float t = card.tubeArc * (static_cast<float>(e) / static_cast<float>(across) - 0.5f);
            row.push_back(card.center[i] + tubePoint(normal, side, halfW, card.tubeAspect, t).offset);
        }
        return row;
    }
    for (int e = 0; e <= across; ++e) {
        // Across the card, -1..1; curvature bows the middle out along the normal.
        const float x = 2.0f * static_cast<float>(e) / static_cast<float>(across) - 1.0f;
        const float bow = card.curvature * halfW * (1.0f - x * x);
        row.push_back(card.center[i] + side * (x * halfW) + normal * bow);
    }
    return row;
}

void seatCard(CardMesh::Card& card, const ScalpMesh& scalp, const Vec3f& root, float clearance, float rigid) {
    card.base.clear();
    if (card.center.empty() || scalp.triangleCount() == 0) return;
    if (scalp.closestPoint(root, 4.0f * clearance).root.face < 0) return;  // not rooted on this scalp
    // Each vertex along the scalp's normal under it: the strands' roots all lie on the skin, so the
    // row lies down on it whichever way the card turns above (the fit tilts it, the curvature bows
    // it). A few times: the smooth normal leans off a coarse mesh's flat faces, so a move from
    // well off the skin (a card out on its strands' outer side) lands near, not on.
    const std::vector<Vec3f> row = cardRow(card, 0);
    std::vector<Vec3f> seated = row, up(row.size());
    float lowest = std::numeric_limits<float>::max();
    for (std::size_t e = 0; e < row.size(); ++e) {
        for (int pass = 0; pass < 4; ++pass) {
            const SurfacePoint near = scalp.closestPoint(seated[e]);
            up[e] = near.normal.normalized();
            const float height = (seated[e] - near.position).dot(up[e]);
            if (pass == 0) lowest = std::min(lowest, height);
            seated[e] += up[e] * (clearance - height);
        }
    }
    // A row standing on edge (a crossed card turned up off hair lying down) would fold onto a
    // line: it moves as a whole instead, its lowest vertex onto the skin.
    if ((seated.back() - seated.front()).length() < 0.5f * (row.back() - row.front()).length()) {
        for (std::size_t e = 0; e < row.size(); ++e) seated[e] = row[e] + up[e] * (clearance - lowest);
    } else if (rigid > 0.0f) {
        // Toward the row moved as a whole with the feather's shaft planted at its original
        // root. Using the lowest outer edge here floats an asymmetric feather's shaft by as
        // much as the row tilts across its width (most visibly when featherFlatten is 1).
        Vec3f mean{};
        for (const Vec3f& u : up) mean += u;
        mean = mean.normalized();
        const Vec3f anchor = card.center.front() + card.side.front() * card.spineOffset;
        const SurfacePoint near = scalp.closestPoint(root);
        const float height = (anchor - near.position).dot(mean);
        const float r = std::min(rigid, 1.0f);
        for (std::size_t e = 0; e < row.size(); ++e) seated[e] = seated[e] * (1.0f - r) + (row[e] + mean * (clearance - height)) * r;
    }
    card.base = std::move(seated);
}

float textureWidthOf(const CardMesh::Card& card) {
    if (card.textureWidth > 0.0f) return card.textureWidth;
    float width = 0.0f;
    for (const float w : CardShape(card).width) width = std::max(width, w);
    return std::max(width, 1e-9f);
}

float finalWidthAt(const CardMesh::Card& card, float along) {
    if (card.width.empty()) return textureWidthOf(card);
    if (card.width.size() == 1) return std::max(card.width.front(), 1e-9f);
    const float at = std::clamp(along, 0.0f, 1.0f) * static_cast<float>(card.width.size() - 1);
    const std::size_t a = std::min(static_cast<std::size_t>(at), card.width.size() - 2);
    const float t = at - static_cast<float>(a);
    return std::max(card.width[a] * (1.0f - t) + card.width[a + 1] * t, 1e-9f);
}

CardPoint projectOntoCard(const CardMesh::Card& cardIn, const Vec3f& p, int& segment, bool finalMesh) {
    const CardShape card(cardIn, finalMesh);
    const int last = static_cast<int>(card.center.size()) - 2;  // last segment
    int lo = 0, hi = last;
    if (segment >= 0) lo = std::max(0, segment - 3), hi = std::min(last, segment + 3);
    float best = std::numeric_limits<float>::max();
    int bestK = lo;
    float bestT = 0.0f;
    bool bestBetween = false;
    // Each station's plane faces the way the card runs there (the mean of its segments' ways):
    // a point lies between two stations' planes, and how far between them is how far along.
    // The planes turn from station to station, so a point off the middle (a wide card's edge)
    // moves along smoothly as it moves; projecting it onto the nearest segment instead pinned
    // every point outside a bend to the station there, and the strands baked in steps.
    auto facing = [&](int k) {
        const std::size_t a = static_cast<std::size_t>(std::max(k - 1, 0));
        const std::size_t b = static_cast<std::size_t>(std::min(k + 1, last + 1));
        return (card.center[b] - card.center[a]).normalized();
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = lo; k <= hi; ++k) {
            const Vec3f& a = card.center[static_cast<std::size_t>(k)];
            const Vec3f& b = card.center[static_cast<std::size_t>(k) + 1];
            const float da = (p - a).dot(facing(k)), db = (p - b).dot(facing(k + 1));
            // Past the root or the tip: keep going (it lands outside the texture).
            float t = da - db > 1e-20f ? da / (da - db) : 0.5f;
            const float clamped = std::clamp(t, k == 0 ? -10.0f : 0.0f, k == last ? 10.0f : 1.0f);
            const Vec3f q = a + (b - a) * clamped;
            // Between this segment's planes beats any other's nearest end (outside a bend, a
            // neighbour's end station can be nearer than the point's own segment).
            const float d2 = (p - q).dot(p - q);
            const bool between = clamped == t;
            if ((between && !bestBetween) || (between == bestBetween && d2 < best)) {
                best = d2, bestK = k, bestT = clamped, bestBetween = between;
            }
        }
        // Near the window's edge: it may be better elsewhere, search it all.
        if (segment < 0 || (bestK != lo && bestK != hi) || (lo == 0 && hi == last)) break;
        lo = 0, hi = last;
    }
    segment = bestK;
    const auto i = static_cast<std::size_t>(bestK);
    const float w = std::clamp(bestT, 0.0f, 1.0f);
    CardPoint out;
    out.side = (card.side[i] * (1.0f - w) + card.side[i + 1] * w).normalized();
    out.normal = (card.normal[i] * (1.0f - w) + card.normal[i + 1] * w).normalized();
    out.forward = (card.center[i + 1] - card.center[i]).normalized();
    out.width = std::max(card.width[i] * (1.0f - w) + card.width[i + 1] * w, 1e-9f);
    out.textureWidth = textureWidthOf(cardIn);
    const Vec3f q = card.center[i] + (card.center[i + 1] - card.center[i]) * bestT;
    const Vec3f d = p - q;
    out.offset = d.dot(out.side);
    out.across = out.offset / out.textureWidth + 0.5f;
    out.depth = d.dot(out.normal);
    out.along = cardIn.stationAlong.size() == card.center.size()
                    ? cardIn.stationAlong[i] * (1.0f - bestT) + cardIn.stationAlong[i + 1] * bestT
                    : (static_cast<float>(bestK) + bestT) / static_cast<float>(card.center.size() - 1);
    if (cardIn.tubeArc > 0.0f) {
        // A tube: across is the way around (the angle from the normal toward the side over the
        // arc), depth how far out of the tube's surface (its radius here), and the frame the
        // surface's there: side the way around, normal straight out. The texture's width at this
        // station is the arc's length at the surface, so a strand bakes as wide as it is.
        const Vec3f radial = d - out.forward * d.dot(out.forward);
        const float radius = 0.5f * out.width, aspect = std::max(cardIn.tubeAspect, 1e-3f);
        // The ellipse's parameter of the direction the point lies in, and the surface there.
        const float t = pm::atan2(radial.dot(out.side) / aspect, radial.dot(out.normal));
        const TubePoint surface = tubePoint(out.normal, out.side, radius, aspect, t);
        out.across = t / cardIn.tubeArc + 0.5f;
        out.depth = radial.length() - surface.offset.length();
        out.textureWidth = std::max(cardIn.tubeArc * surface.speed, 1e-9f);
        out.offset = (out.across - 0.5f) * out.textureWidth;
        out.side = surface.around;
        out.normal = surface.outward;
        return out;
    }
    if (cardIn.spine) {
        // A spine card: across and along stay the flat card's (its vertices keep the flat UVs),
        // depth and frame are the folded half's, so the bake's depth and tangent are in the frame
        // the half shades with and a point's distance is to the half it lies over.
        const SpineHalf half = spineHalf(card, cardIn.spineOffset, out.offset, i, w, out.side, out.normal);
        out.depth = (d - out.side * cardIn.spineOffset).dot(half.normal);
        out.side = half.across;
        out.normal = half.normal;
    }
    return out;
}

StrandColors colorsOf(const StrandSet& s, const std::array<float, 3>& fallback) {
    StrandColors c;
    // A color map is material input, not lighting output: preserve the authored base color.
    c.colors = strandColors(s);
    c.points = pointColors(s);
    const std::vector<float>* iridescence = s.pointChannel(kIridescenceChannel);
    if (iridescence && iridescence->size() == s.pointCount()) c.iridescence = *iridescence;
    c.fallback = fallback;
    return c;
}

std::array<float, 2> cardIridescence(const CardMesh::Card& card, const StrandSet& s, const StrandColors& colors) {
    if (colors.iridescence.empty() || card.strands.empty()) return {0.0f, 0.0f};
    const std::vector<float>* side = card.feather >= 0 ? s.strandChannel("side") : nullptr;
    if (side && side->size() != s.strandCount()) side = nullptr;
    // Every strand on a hair card (a sample of 64); a feather's barbs all, by vane.
    const std::size_t stride = side ? 1 : std::max<std::size_t>(1, card.strands.size() / 64);
    float sum[2] = {0.0f, 0.0f}, count[2] = {0.0f, 0.0f};
    for (std::size_t m = 0; m < card.strands.size(); m += stride) {
        const std::size_t si = card.strands[m];
        const float vane = side ? (*side)[si] : 0.0f;
        for (int j = 0; j < kCardColorStations; ++j) {
            const float v = colors.iridescenceAt(s, si, static_cast<float>(j) / static_cast<float>(kCardColorStations - 1));
            for (int k = 0; k < 2; ++k) {
                if ((k == 0 && vane > 0.5f) || (k == 1 && vane < -0.5f)) continue;  // the other vane's
                sum[k] += v;
                count[k] += 1.0f;
            }
        }
    }
    return {count[0] > 0.0f ? sum[0] / count[0] : 0.0f, count[1] > 0.0f ? sum[1] / count[1] : 0.0f};
}

std::vector<std::array<float, 3>> cardColors(const CardMesh::Card& card, const StrandSet& s, const StrandColors& colors) {
    std::vector<std::array<float, 3>> out(kCardColorStations, std::array<float, 3>{0.0f, 0.0f, 0.0f});
    if (card.strands.empty()) return {};
    const std::size_t stride = std::max<std::size_t>(1, card.strands.size() / 64);
    float count = 0.0f;
    for (std::size_t m = 0; m < card.strands.size(); m += stride) {
        for (int j = 0; j < kCardColorStations; ++j) {
            const auto c = colors.at(s, card.strands[m], static_cast<float>(j) / static_cast<float>(kCardColorStations - 1));
            for (int k = 0; k < 3; ++k) out[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] += c[static_cast<std::size_t>(k)];
        }
        count += 1.0f;
    }
    for (auto& c : out) {
        for (float& v : c) v /= count;
    }
    return out;
}

std::array<float, 3> cardTint(const CardMesh::Card& card, float along) {
    const auto& own = card.colors;
    const auto& texture = card.textureColors;
    if (own.size() != static_cast<std::size_t>(kCardColorStations) || texture.size() != own.size()) return {1.0f, 1.0f, 1.0f};
    const float f = std::clamp(along, 0.0f, 1.0f) * static_cast<float>(kCardColorStations - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(f), static_cast<std::size_t>(kCardColorStations - 2));
    const float w = f - static_cast<float>(i);
    std::array<float, 3> tint{};
    for (std::size_t k = 0; k < 3; ++k) {
        const float a = own[i][k] * (1.0f - w) + own[i + 1][k] * w;
        const float b = texture[i][k] * (1.0f - w) + texture[i + 1][k] * w;
        tint[k] = b > 1e-4f ? std::clamp(a / b, 0.0f, kCardTintMax) : 1.0f;
    }
    return tint;
}

float distance2(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.0f;
    for (std::size_t k = 0; k < a.size(); ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
    return d;
}

std::vector<Frame> cardFrames(const std::vector<Vec3f>& center, const ScalpMesh* scalp) {
    Vec3f up{0, 1, 0};
    if (scalp) up = scalp->closestPoint(center.front()).normal;
    const Vec3f t0 = (center[1] - center[0]).normalized();
    up = up - t0 * up.dot(t0);
    if (up.length() < 1e-4f) up = anyPerpendicular(t0);
    return rotationMinimizingFrames(center.data(), center.size(), up.normalized());
}

std::vector<int> kMeans(const std::vector<std::vector<float>>& f, std::size_t k, int iterations) {
    const std::size_t n = f.size();
    std::vector<int> group(n, 0);
    if (n == 0 || k <= 1) return group;
    k = std::min(k, n);
    const std::size_t dims = f[0].size();
    std::vector<float> mean(dims, 0.0f);
    for (const auto& x : f) {
        for (std::size_t d = 0; d < dims; ++d) mean[d] += x[d] / static_cast<float>(n);
    }
    std::size_t first = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (distance2(f[i], mean) < distance2(f[first], mean)) first = i;
    }
    std::vector<std::vector<float>> centers{f[first]};
    std::vector<float> nearest(n, std::numeric_limits<float>::max());
    while (centers.size() < k) {
        std::size_t far = 0;
        float farD = -1.0f;
        for (std::size_t i = 0; i < n; ++i) {
            nearest[i] = std::min(nearest[i], distance2(f[i], centers.back()));
            if (nearest[i] > farD) farD = nearest[i], far = i;
        }
        if (farD <= 0.0f) break;  // the rest coincide
        centers.push_back(f[far]);
    }
    k = centers.size();
    for (int it = 0; it < iterations; ++it) {
        bool moved = false;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t best = 0;
            float bestD = distance2(f[i], centers[0]);
            for (std::size_t c = 1; c < k; ++c) {
                const float d = distance2(f[i], centers[c]);
                if (d < bestD) bestD = d, best = c;
            }
            moved = moved || static_cast<int>(best) != group[i];
            group[i] = static_cast<int>(best);
        }
        std::vector<std::vector<float>> sums(k, std::vector<float>(dims, 0.0f));
        std::vector<float> counts(k, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t d = 0; d < dims; ++d) sums[static_cast<std::size_t>(group[i])][d] += f[i][d];
            counts[static_cast<std::size_t>(group[i])] += 1.0f;
        }
        for (std::size_t c = 0; c < k; ++c) {
            if (counts[c] > 0.0f) {
                for (std::size_t d = 0; d < dims; ++d) centers[c][d] = sums[c][d] / counts[c];
            }
        }
        if (!moved && it > 0) break;
    }
    std::vector<int> renumber(k, -1);
    int next = 0;
    for (int& g : group) {
        int& r = renumber[static_cast<std::size_t>(g)];
        if (r < 0) r = next++;
        g = r;
    }
    return group;
}

}  // namespace detail

namespace {

// Cluster index per strand (-1: none). Strands `skip` marks (feathers carded apart) are in none.
std::vector<int> clusterStrands(const StrandSet& s, const CardSettings& settings, const std::vector<char>& skip) {
    std::vector<int> cluster(s.strandCount(), -1);
    const auto skipped = [&](std::size_t i) { return !skip.empty() && skip[i]; };
    if (const std::vector<float>* ids = settings.channel.empty() ? nullptr : s.strandChannel(settings.channel)) {
        std::map<long long, int> remap;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            if (skipped(i)) continue;
            const long long key = std::llround((*ids)[i]);
            if (key < 0) continue;  // not in a clump
            auto it = remap.emplace(key, static_cast<int>(remap.size())).first;
            cluster[i] = it->second;
        }
        // A channel none of these strands is in (hair merged with feathers, whose barbs brought
        // clumpId along): as good as none.
        if (!remap.empty()) return cluster;
    }
    // No clump channel: spread-out centers (stable order by hashed ID), nearest root; as many as
    // the budget gives cards of cardsPerClump x layers each (no budget: 60).
    const int perGroup = std::max(settings.cardsPerClump, 1) * std::clamp(settings.layers, 1, 3);
    const std::size_t want =
        static_cast<std::size_t>(settings.maxCards > 0 ? std::max(settings.maxCards / perGroup, 1) : 60);
    std::vector<std::pair<std::uint64_t, std::size_t>> order;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (!skipped(i)) order.emplace_back(hash64(s.ids()[i] ^ 0xCA7D5ull), i);
    }
    std::sort(order.begin(), order.end());
    std::vector<Vec3f> centers;
    // Farthest-point-ish: take candidates in hashed order, skipping ones too near a center.
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (skipped(i)) continue;
        const Vec3f r = s.strandPoints(i)[0];
        lo = {std::min(lo.x, r.x), std::min(lo.y, r.y), std::min(lo.z, r.z)};
        hi = {std::max(hi.x, r.x), std::max(hi.y, r.y), std::max(hi.z, r.z)};
    }
    const float extent = (hi - lo).length();
    const float spacing = extent / std::sqrt(static_cast<float>(want)) * 0.5f;
    for (const auto& [key, i] : order) {
        if (centers.size() >= want) break;
        const Vec3f r = s.strandPoints(i)[0];
        bool near = false;
        for (const Vec3f& c : centers) near = near || (c - r).length() < spacing;
        if (!near) centers.push_back(r);
    }
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (skipped(i)) continue;
        const Vec3f r = s.strandPoints(i)[0];
        float best = 1e30f;
        for (std::size_t c = 0; c < centers.size(); ++c) {
            const float d = (centers[c] - r).dot(centers[c] - r);
            if (d < best) {
                best = d;
                cluster[i] = static_cast<int>(c);
            }
        }
    }
    return cluster;
}

// At most `maxCards` clusters: whole clusters join their nearest of `maxCards`
// spread-out ones (farthest-point picks over the clusters' root centers), so
// a lower LOD has fewer, wider cards that still follow the clumps.
void mergeClusters(const StrandSet& s, std::vector<int>& cluster, int maxCards, int minStrands) {
    int count = 0;
    for (const int c : cluster) count = std::max(count, c + 1);
    std::vector<Vec3f> center(static_cast<std::size_t>(count));
    std::vector<int> members(static_cast<std::size_t>(count), 0);
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (cluster[i] < 0) continue;
        center[static_cast<std::size_t>(cluster[i])] += s.strandPoints(i)[0];
        ++members[static_cast<std::size_t>(cluster[i])];
    }
    std::vector<int> live;
    for (int c = 0; c < count; ++c) {
        if (members[static_cast<std::size_t>(c)] > 0) {
            center[static_cast<std::size_t>(c)] = center[static_cast<std::size_t>(c)] / static_cast<float>(members[static_cast<std::size_t>(c)]);
            live.push_back(c);
        }
    }
    int cards = 0;
    for (const int c : live) cards += members[static_cast<std::size_t>(c)] >= minStrands ? 1 : 0;
    if (maxCards <= 0 || cards <= maxCards || live.empty()) return;
    // Farthest-point picks, starting from the biggest cluster.
    std::vector<int> picks{*std::max_element(live.begin(), live.end(), [&](int a, int b) {
        return members[static_cast<std::size_t>(a)] < members[static_cast<std::size_t>(b)];
    })};
    std::vector<float> nearest(static_cast<std::size_t>(count), std::numeric_limits<float>::max());
    while (static_cast<int>(picks.size()) < maxCards) {
        const Vec3f& last = center[static_cast<std::size_t>(picks.back())];
        int far = -1;
        float farD = -1.0f;
        for (const int c : live) {
            float& d = nearest[static_cast<std::size_t>(c)];
            d = std::min(d, (center[static_cast<std::size_t>(c)] - last).dot(center[static_cast<std::size_t>(c)] - last));
            if (d > farD) farD = d, far = c;
        }
        if (far < 0 || farD <= 0.0f) break;
        picks.push_back(far);
    }
    std::vector<int> to(static_cast<std::size_t>(count), -1);
    for (const int c : live) {
        float best = std::numeric_limits<float>::max();
        for (std::size_t p = 0; p < picks.size(); ++p) {
            const Vec3f d = center[static_cast<std::size_t>(c)] - center[static_cast<std::size_t>(picks[p])];
            if (d.dot(d) < best) best = d.dot(d), to[static_cast<std::size_t>(c)] = static_cast<int>(p);
        }
    }
    for (int& c : cluster) {
        if (c >= 0) c = to[static_cast<std::size_t>(c)];
    }
}

// A strand sample's offset from a card's median curve, at its nearest station.
struct Offset {
    std::size_t station;
    Vec3f d;
};

// Samples along each member (by arc length), each at its nearest station of `center`
// (the samples go root to tip, so the nearest station only ever moves up).
// `ahead` (0: the next one only): how many stations on a sample may skip to the nearest, for a
// curve that winds (a braid section's: the next station can lie farther than one past it).
// The station nearest p of all (where a strand that joins a braid partway along starts).
std::size_t nearestStation(const std::vector<Vec3f>& center, const Vec3f& p) {
    std::size_t best = 0;
    float bestD = std::numeric_limits<float>::max();
    for (std::size_t j = 0; j < center.size(); ++j) {
        const float d = (center[j] - p).dot(center[j] - p);
        if (d < bestD) bestD = d, best = j;
    }
    return best;
}

std::size_t nearestStationAhead(const std::vector<Vec3f>& center, const Vec3f& p, std::size_t k, std::size_t ahead) {
    if (ahead == 0) {
        while (k + 1 < center.size() && (center[k + 1] - p).dot(center[k + 1] - p) <= (center[k] - p).dot(center[k] - p)) ++k;
        return k;
    }
    std::size_t best = k;
    float bestD = (center[k] - p).dot(center[k] - p);
    for (std::size_t j = k + 1; j < std::min(center.size(), k + ahead + 1); ++j) {
        const float d = (center[j] - p).dot(center[j] - p);
        if (d <= bestD) bestD = d, best = j;
    }
    return best;
}

std::vector<Offset> strandOffsets(const StrandSet& s, const std::vector<std::size_t>& members, const std::vector<Vec3f>& center,
                                  std::size_t samples, std::size_t ahead = 0) {
    std::vector<Offset> offsets;
    offsets.reserve(members.size() * samples);
    std::vector<Vec3f> pts(samples);
    for (const std::size_t i : members) {
        resampleByArcLengthInto(s.strandPoints(i), s.strandPointCount(i), pts.data(), samples);
        std::size_t k = ahead > 0 ? nearestStation(center, pts.front()) : 0;
        for (const Vec3f& p : pts) {
            k = nearestStationAhead(center, p, k, ahead);
            offsets.push_back({k, p - center[k]});
        }
    }
    return offsets;
}

// The value at `share` (0..1) of `values` sorted (they are reordered).
float percentile(std::vector<float>& values, float share) {
    if (values.empty()) return 0.0f;
    const auto at = static_cast<std::size_t>(std::lround(share * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

// How much of the members a card fits across: the rest (a few strands straying sideways from
// the others) are drawn at the card's edge rather than widen it, so the cards keep to the groom's
// envelope however many strands each holds (a lower level of detail's cards hold more, and the
// farthest of more strands strays farther). Along its length, however, a card always reaches its
// farthest member tip.
constexpr float kFitShare = 0.9f;
// Where across its strands' thickness a ribbon lies: the depth (out of the scalp) a quarter of
// them lie beyond.
constexpr float kOuterShell = 0.75f;

// The members' median curve at kRefStations (each station the median of theirs, axis by axis:
// a few strands leaving the rest don't pull it off them), carried on past its tip along its last
// direction as far as the farthest member tip reaches.
std::vector<Vec3f> medianCurve(const StrandSet& s, const std::vector<std::size_t>& members) {
    std::vector<Vec3f> center(kRefStations, Vec3f{});
    std::vector<Vec3f> pts(kRefStations);
    std::vector<std::vector<float>> axes(kRefStations * 3);
    for (auto& a : axes) a.reserve(members.size());
    for (const std::size_t i : members) {
        resampleByArcLengthInto(s.strandPoints(i), s.strandPointCount(i), pts.data(), kRefStations);
        for (std::size_t k = 0; k < kRefStations; ++k) {
            axes[k * 3].push_back(pts[k].x), axes[k * 3 + 1].push_back(pts[k].y), axes[k * 3 + 2].push_back(pts[k].z);
        }
    }
    for (std::size_t k = 0; k < kRefStations; ++k) {
        center[k] = {percentile(axes[k * 3], 0.5f), percentile(axes[k * 3 + 1], 0.5f), percentile(axes[k * 3 + 2], 0.5f)};
    }
    const float meanLength = arcLength(center);
    if (meanLength <= 1e-9f) return center;
    const Vec3f tip = (center.back() - center[kRefStations - 2]).normalized();
    std::vector<float> reaches;
    reaches.reserve(members.size());
    for (const std::size_t i : members) {
        const Vec3f& end = s.strandPoints(i)[s.strandPointCount(i) - 1];
        reaches.push_back((end - center.back()).dot(tip));
    }
    const float reach = *std::max_element(reaches.begin(), reaches.end());
    if (reach > -0.005f * meanLength) {  // a hair past them, so no tip lands on the card's end
        std::vector<Vec3f> longer = center;
        longer.push_back(center.back() + tip * (reach + 0.01f * meanLength));
        center = resampleByArcLength(longer.data(), longer.size(), kRefStations);
    }
    return center;
}

// A clump's strands split into `layers` depth bands (by how far out of the scalp they
// run, along the clump's normal), each band into groups of strands lying together (k-means
// over their offsets from the clump's median curve at a few heights), `groups` in all.
// Returns the groups (member lists) with their layer.
struct StrandGroup {
    std::vector<std::size_t> members;
    int layer = 0;
};
std::vector<StrandGroup> splitClump(const StrandSet& s, const std::vector<std::size_t>& members, const ScalpMesh* scalp,
                                    int groups, int layers) {
    if (groups <= 1 && layers <= 1) return {{members, 0}};
    const std::vector<Vec3f> center = medianCurve(s, members);
    const std::vector<Frame> frames = cardFrames(center, scalp);
    // Per strand: its offset across the clump (in the frame: binormal, normal) at a few
    // stations up the clump, the last known one carried up for the short ones.
    constexpr std::size_t kSamples = 16;
    const std::size_t stationsAt[4] = {kRefStations / 4, kRefStations / 2, 3 * kRefStations / 4, kRefStations - 1};
    const std::vector<Offset> offsets = strandOffsets(s, members, center, kSamples);
    std::vector<std::vector<float>> features(members.size(), std::vector<float>(8, 0.0f));
    std::vector<float> depth(members.size(), 0.0f);
    for (std::size_t m = 0; m < members.size(); ++m) {
        const Offset* o = &offsets[m * kSamples];
        std::size_t at = 0;
        float meanDepth = 0.0f;
        for (std::size_t j = 0; j < 4; ++j) {
            while (at + 1 < kSamples && o[at + 1].station <= stationsAt[j]) ++at;
            const Frame& f = frames[o[at].station];
            features[m][j * 2] = o[at].d.dot(f.binormal);
            features[m][j * 2 + 1] = o[at].d.dot(f.normal);
        }
        for (std::size_t j = 0; j < kSamples; ++j) meanDepth += o[j].d.dot(frames[o[j].station].normal) / static_cast<float>(kSamples);
        depth[m] = meanDepth;
    }
    // Depth bands of equal count, innermost first.
    const int bands = std::clamp(std::min(layers, groups), 1, 3);
    std::vector<std::size_t> byDepth(members.size());
    for (std::size_t m = 0; m < members.size(); ++m) byDepth[m] = m;
    std::stable_sort(byDepth.begin(), byDepth.end(), [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });
    std::vector<StrandGroup> out;
    for (int band = 0; band < bands; ++band) {
        const std::size_t from = members.size() * static_cast<std::size_t>(band) / static_cast<std::size_t>(bands);
        const std::size_t to = members.size() * static_cast<std::size_t>(band + 1) / static_cast<std::size_t>(bands);
        if (to <= from) continue;
        const int want = groups / bands + (band >= bands - groups % bands ? 1 : 0);  // the outer bands get the odd one
        std::vector<std::vector<float>> f;
        for (std::size_t j = from; j < to; ++j) f.push_back(features[byDepth[j]]);
        const std::vector<int> group = kMeans(f, static_cast<std::size_t>(std::max(want, 1)), 8);
        int count = 0;
        for (const int g : group) count = std::max(count, g + 1);
        std::vector<StrandGroup> made(static_cast<std::size_t>(count));
        for (std::size_t j = 0; j < group.size(); ++j) {
            made[static_cast<std::size_t>(group[j])].members.push_back(members[byDepth[from + j]]);
            made[static_cast<std::size_t>(group[j])].layer = band;
        }
        for (StrandGroup& g : made) {
            std::sort(g.members.begin(), g.members.end());
            out.push_back(std::move(g));
        }
    }
    return out;
}

// Cards per clump within the budget: `want` each, no more than its strands allow
// (minStrands per card); over `maxCards` in all, the clumps with the fewest strands per
// card give up cards first, down to one each.
std::vector<int> allocateCards(const std::vector<std::size_t>& counts, int want, int minStrands, int maxCards) {
    std::vector<int> k(counts.size(), 0);
    long long total = 0;
    for (std::size_t c = 0; c < counts.size(); ++c) {
        if (static_cast<int>(counts[c]) < minStrands) continue;
        k[c] = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(std::max(want, 1)), counts[c] / static_cast<std::size_t>(minStrands)));
        k[c] = std::max(k[c], 1);
        total += k[c];
    }
    if (maxCards <= 0 || total <= maxCards) return k;
    // Fewest strands per card first (a heap of (strands per card, clump)).
    using Item = std::pair<float, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (std::size_t c = 0; c < counts.size(); ++c) {
        if (k[c] > 1) heap.emplace(static_cast<float>(counts[c]) / static_cast<float>(k[c]), c);
    }
    while (total > maxCards && !heap.empty()) {
        const std::size_t c = heap.top().second;
        heap.pop();
        --k[c];
        --total;
        if (k[c] > 1) heap.emplace(static_cast<float>(counts[c]) / static_cast<float>(k[c]), c);
    }
    return k;
}

// Points hashed into cells, for near-neighbour lookups.
class PointGrid {
public:
    PointGrid(const std::vector<Vec3f>& points, float cell) : points_(points), cell_(std::max(cell, 1e-6f)) {
        for (std::size_t i = 0; i < points.size(); ++i) cells_[key(points[i])].push_back(i);
    }
    // The nearest point within `radius` (-1: none), and its distance.
    std::pair<long long, float> nearest(const Vec3f& p, float radius) const {
        long long best = -1;
        float bestD = radius * radius;
        const int r = static_cast<int>(std::ceil(radius / cell_));
        const int cx = static_cast<int>(std::floor(p.x / cell_)), cy = static_cast<int>(std::floor(p.y / cell_)),
                  cz = static_cast<int>(std::floor(p.z / cell_));
        for (int z = cz - r; z <= cz + r; ++z) {
            for (int y = cy - r; y <= cy + r; ++y) {
                for (int x = cx - r; x <= cx + r; ++x) {
                    const auto it = cells_.find(pack(x, y, z));
                    if (it == cells_.end()) continue;
                    for (const std::size_t i : it->second) {
                        const float d = (points_[i] - p).dot(points_[i] - p);
                        if (d < bestD) bestD = d, best = static_cast<long long>(i);
                    }
                }
            }
        }
        return {best, std::sqrt(bestD)};
    }
    // Every point within `radius`.
    std::vector<std::size_t> within(const Vec3f& p, float radius) const {
        std::vector<std::size_t> out;
        const int r = static_cast<int>(std::ceil(radius / cell_));
        const int cx = static_cast<int>(std::floor(p.x / cell_)), cy = static_cast<int>(std::floor(p.y / cell_)),
                  cz = static_cast<int>(std::floor(p.z / cell_));
        for (int z = cz - r; z <= cz + r; ++z) {
            for (int y = cy - r; y <= cy + r; ++y) {
                for (int x = cx - r; x <= cx + r; ++x) {
                    const auto it = cells_.find(pack(x, y, z));
                    if (it == cells_.end()) continue;
                    for (const std::size_t i : it->second) {
                        if ((points_[i] - p).dot(points_[i] - p) <= radius * radius) out.push_back(i);
                    }
                }
            }
        }
        return out;
    }

private:
    static std::uint64_t pack(int x, int y, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42) ^
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 21) ^ static_cast<std::uint32_t>(z);
    }
    std::uint64_t key(const Vec3f& p) const {
        return pack(static_cast<int>(std::floor(p.x / cell_)), static_cast<int>(std::floor(p.y / cell_)),
                    static_cast<int>(std::floor(p.z / cell_)));
    }
    const std::vector<Vec3f>& points_;
    float cell_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> cells_;
};

// How a card is built from its strands, apart from the settings: which of a cross it is, and
// its ruffle sign.
// A cornrow's tube: the part of a tube over the scalp (225 degrees: its edges a little under its
// center line, toward the skin).
constexpr float kCornrowArc = 3.92699082f;

struct CardSpec {
    int clump = -1;
    int layer = 0;
    int group = -1;     // the group of strands it was made from (its cross's cards share it)
    int turn = 0;       // which card of a cross (0: the first)
    int crossing = 0;   // cards in the cross (0: decide from the strands; set: rebuild that one card)
    int parity = 0;     // ruffle: even cards push out, odd ones in
    bool ruffled = false;  // one of several cards of its clump (ruffle applies)
    bool flyaway = false;
    // A braid's cards: exactly `cards` around the braid's axis (a cross along a hanging
    // braid; one ribbon facing out of the scalp for a cornrow), whatever the settings'
    // cross style, and no fit (woven strands have no axis across to follow).
    int braid = -1;
    int cards = 0;
    float tubeArc = 0.0f;  // a braid's tube (CardSettings::braidCards): the card's Card::tubeArc
    int section = -1;      // a braid section's tube: its section (Card::braidSection)
    bool feed = false;     // a braid's feed card (Card::braidFeed): made of its strands' way in
    // A guide's cards (CardSettings::guideSpines): along this curve instead of the strands' median.
    const std::vector<Vec3f>* spine = nullptr;
};

// How far p lies from the card's surface: off its plane (bowed by its curvature), and
// past its edges or ends, to them.
float pointCardDistance(const CardMesh::Card& card, float cardLength, const Vec3f& p, int& segment) {
    const CardPoint cp = projectOntoCard(card, p, segment);
    if (card.tubeArc > 0.0f) {  // a tube: off its surface, and past its ends
        const float dy = cardLength * (cp.along < 0.0f ? -cp.along : cp.along > 1.0f ? cp.along - 1.0f : 0.0f);
        return std::sqrt(cp.depth * cp.depth + dy * dy);
    }
    // Across, in the station's own width (the card's physical edges), not the texture's.
    const float x = cp.offset / cp.width;  // -0.5 .. 0.5 on the card
    const float dx = cp.width * std::max(0.0f, std::abs(x) - 0.5f);
    const float dy = cardLength * (cp.along < 0.0f ? -cp.along : cp.along > 1.0f ? cp.along - 1.0f : 0.0f);
    const float bow = card.curvature * 0.5f * cp.width * (1.0f - 4.0f * x * x);
    const float dz = cp.depth - (std::abs(x) <= 0.5f ? bow : 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The mean distance of a strand (sampled along it) from a card.
float strandCardDistance(const StrandSet& s, std::size_t si, const CardMesh::Card& card) {
    constexpr std::size_t kSamples = 8;
    const std::vector<Vec3f> pts = resampleByArcLength(s.strandPoints(si), s.strandPointCount(si), kSamples);
    const float length = arcLength(CardShape(card).center);
    int segment = -1;
    float sum = 0.0f;
    for (const Vec3f& p : pts) sum += pointCardDistance(card, length, p, segment);
    return sum / static_cast<float>(kSamples);
}

// The scalp's normal where the members grow, averaged (`fallback` when none is rooted on it).
Vec3f cardRootNormal(const StrandSet& s, const std::vector<std::size_t>& members, const ScalpMesh& scalp, const Vec3f& fallback) {
    Vec3f sum{0.0f, 0.0f, 0.0f};
    for (const std::size_t i : members) {
        const StrandRoot& root = s.roots()[i];
        if (root.face >= 0 && static_cast<std::size_t>(root.face) < scalp.triangleCount()) sum += scalp.normal(root);
    }
    return sum.length() > 1e-6f ? sum.normalized() : fallback;
}

// The strands' lit drawing's normal (display.cpp's strandFrame) at station k of `center`: the
// root normal `up` with half its part along the curve there removed.
Vec3f strandLookNormal(const Vec3f& up, const std::vector<Vec3f>& center, std::size_t k) {
    const std::size_t last = center.size() - 1;
    const Vec3f forward = (center[std::min(k + 1, last)] - center[k > 0 ? k - 1 : 0]).normalized();
    const Vec3f n = up - forward * (0.5f * up.dot(forward));
    return n.length() > 1e-4f ? n.normalized() : up;
}

// A braid's tube (CardSpec::tubeArc), built at kRefStations along its strands' median curve:
// a closed tube for a hanging braid, the upper part of one for a cornrow (open toward the scalp
// it lies on). Its radius at each station is where most of its strands lie out from the curve
// there (the fuzz and the strays bake onto its surface instead of puffing it up), smoothed, and
// never far over the braid's typical thickness (the hair gathering into it at the scalp, a
// fanned end).
CardMesh::Card buildTubeCard(const StrandSet& s, const std::vector<std::size_t>& members, const ScalpMesh* scalp,
                             const CardSettings& settings, const CardSpec& spec, const std::vector<Vec3f>& center) {
    constexpr float kTubeShare = 0.85f;  // the share of its strands a station's radius holds
    const std::size_t refStations = center.size();  // (kRefStations; a section's tube more: sectionCurve)
    CardMesh::Card card;
    card.braidSection = spec.section;
    card.strands = members;
    card.braid = spec.braid;
    card.tubeArc = spec.tubeArc;
    card.stations = std::max(settings.segments, 1) + 1;
    const int sides = std::clamp(settings.braidSides, 3, 16);
    const float closed = 6.28318530718f;
    card.across = card.tubeArc >= closed - 1e-4f
                      ? sides
                      : std::max(2, static_cast<int>(std::lround(static_cast<float>(sides) * card.tubeArc / closed)));
    card.refCenter = center;
    const bool hasScalp = scalp && scalp->triangleCount() > 0;
    std::vector<Vec3f> tangent(refStations);
    for (std::size_t k = 0; k < refStations; ++k) {
        tangent[k] = (center[std::min(k + 1, refStations - 1)] - center[k > 0 ? k - 1 : 0]).normalized();
    }
    if (card.tubeArc < closed - 1e-4f && hasScalp) {
        // Open toward the scalp: its normal (the middle of its arc) out of the skin under each station.
        for (std::size_t k = 0; k < refStations; ++k) {
            Vec3f up = scalp->closestPoint(center[k]).normal;
            up = up - tangent[k] * up.dot(tangent[k]);
            if (up.length() < 1e-4f) up = k > 0 ? card.refNormal.back() : anyPerpendicular(tangent[k]);
            card.refNormal.push_back(up.normalized());
            card.refSide.push_back(tangent[k].cross(card.refNormal.back()).normalized());
        }
    } else {
        for (const Frame& f : cardFrames(center, hasScalp ? scalp : nullptr)) {
            card.refNormal.push_back(f.normal);
            card.refSide.push_back(f.binormal);
        }
    }
    // The cross-section's shape: a braid is flatter than it is wide (its sections side by side).
    // The frame turns, station by station, so its side lies along the cross-section's widest way
    // (the principal direction of the strands' offsets, unwrapped and smoothed along the braid; a
    // cornrow's stays over the scalp); the aspect is how much wider than deep it is.
    // (Each sample's nearest station searched a sixteenth of the way ahead: a section winds.)
    const std::vector<Offset> shapeOffsets =
        strandOffsets(s, members, center, std::max<std::size_t>(24, refStations), std::max<std::size_t>(4, refStations / 16));
    if (card.tubeArc >= closed - 1e-4f) {
        std::vector<float> suu(refStations, 0.0f), svv(refStations, 0.0f), suv(refStations, 0.0f);
        for (const Offset& o : shapeOffsets) {
            for (std::size_t k = o.station > 1 ? o.station - 2 : 0; k <= std::min(o.station + 2, refStations - 1); ++k) {
                const float u = o.d.dot(card.refSide[k]), v = o.d.dot(card.refNormal[k]);
                suu[k] += u * u, svv[k] += v * v, suv[k] += u * v;
            }
        }
        std::vector<float> angle(refStations, 0.0f), weight(refStations, 0.0f);
        float previous = 0.0f;
        for (std::size_t k = 0; k < refStations; ++k) {
            const float total = suu[k] + svv[k];
            const float anisotropy = total > 1e-20f ? std::sqrt((suu[k] - svv[k]) * (suu[k] - svv[k]) + 4.0f * suv[k] * suv[k]) / total : 0.0f;
            float a = 0.5f * pm::atan2(2.0f * suv[k], suu[k] - svv[k]);
            while (a - previous > 1.5707963f) a -= 3.14159265f;
            while (a - previous < -1.5707963f) a += 3.14159265f;
            if (anisotropy > 0.1f) previous = a;
            angle[k] = a;
            weight[k] = anisotropy;
        }
        // Smoothed, each station's angle pulled toward its clearer neighbours'.
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<float> next = angle;
            for (std::size_t k = 0; k < refStations; ++k) {
                float sum = 0.0f, w = 0.0f;
                for (std::size_t j = k > 2 ? k - 2 : 0; j <= std::min(k + 2, refStations - 1); ++j) {
                    const float wj = weight[j] + 0.05f;
                    sum += angle[j] * wj, w += wj;
                }
                next[k] = sum / w;
            }
            angle = next;
        }
        for (std::size_t k = 0; k < refStations; ++k) {
            // Turn the frame by the angle: side to the widest way.
            const Vec3f side = card.refSide[k] * pm::cos(angle[k]) + card.refNormal[k] * pm::sin(angle[k]);
            const Vec3f normal = card.refNormal[k] * pm::cos(angle[k]) - card.refSide[k] * pm::sin(angle[k]);
            card.refSide[k] = side.normalized();
            card.refNormal[k] = normal.normalized();
        }
    }
    {
        // How much wider than deep: the strands' spread along the side over their spread along the
        // normal (a share of each), over the whole braid.
        std::vector<float> across, deep;
        for (const Offset& o : shapeOffsets) {
            across.push_back(std::abs(o.d.dot(card.refSide[o.station])));
            deep.push_back(std::abs(o.d.dot(card.refNormal[o.station])));
        }
        const float wide = percentile(across, 0.8f), tall = percentile(deep, 0.8f);
        card.tubeAspect = tall > 1e-9f ? std::clamp(wide / tall, 1.0f, 3.0f) : 1.0f;
    }
    // The members' widths: the tube's surface lies on its outer strands' outer side.
    float strandWidth = 0.0f;
    if (!s.widths().empty()) {
        float sum = 0.0f, n = 0.0f;
        for (const std::size_t i : members) {
            for (std::size_t q = 0; q < s.strandPointCount(i); ++q) sum += s.widths()[s.strandBegin(i) + q], n += 1.0f;
        }
        strandWidth = n > 0.0f && sum / n != 1.0f ? sum / n : 0.0f;
    }
    const float length = std::max(arcLength(center), 1e-9f);
    if (strandWidth <= 0.0f) strandWidth = 0.002f * length;
    // Each strand's farthest point out from the curve at each station (and its neighbours), then
    // the radius that holds kTubeShare of the strands there.
    const std::vector<Offset>& offsets = shapeOffsets;
    const std::size_t samples = offsets.size() / std::max<std::size_t>(members.size(), 1);
    std::vector<std::vector<float>> out(refStations);
    std::vector<float> farthest(refStations);
    for (std::size_t m = 0; m < members.size(); ++m) {
        std::fill(farthest.begin(), farthest.end(), -1.0f);
        for (std::size_t j = 0; j < samples; ++j) {
            const Offset& o = offsets[m * samples + j];
            // How far out in the ellipse's own measure: its half-axis along the normal it would need.
            const float x = o.d.dot(card.refNormal[o.station]), y = o.d.dot(card.refSide[o.station]) / card.tubeAspect;
            const float reach = std::sqrt(x * x + y * y);
            for (std::size_t k = o.station > 0 ? o.station - 1 : 0; k <= std::min(o.station + 1, refStations - 1); ++k) {
                farthest[k] = std::max(farthest[k], reach);
            }
        }
        for (std::size_t k = 0; k < refStations; ++k) {
            if (farthest[k] >= 0.0f) out[k].push_back(farthest[k]);
        }
    }
    std::vector<float> radius(refStations, 0.0f);
    for (std::size_t k = 0; k < refStations; ++k) radius[k] = percentile(out[k], kTubeShare);
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<float> next = radius;
        for (std::size_t k = 1; k + 1 < refStations; ++k) next[k] = 0.25f * radius[k - 1] + 0.5f * radius[k] + 0.25f * radius[k + 1];
        radius = next;
    }
    std::vector<float> sorted = radius;
    const float typical = percentile(sorted, 0.5f);
    const float least = std::max({2.0f * strandWidth, 0.5f * settings.minWidth, 1e-6f});
    for (std::size_t k = 0; k < refStations; ++k) {
        const float r = std::clamp(radius[k], least, std::max(1.6f * typical, least));
        card.refWidth.push_back(2.0f * (r + 0.5f * strandWidth));
    }
    // The texture's width: the arc's length around the widest station (an ellipse's perimeter,
    // Ramanujan's), so its texels are as dense around as along.
    {
        const float a0 = 0.5f * *std::max_element(card.refWidth.begin(), card.refWidth.end()), b0 = a0 * card.tubeAspect;
        const float perimeter = 3.14159265f * (3.0f * (a0 + b0) - std::sqrt((3.0f * a0 + b0) * (a0 + 3.0f * b0)));
        card.textureWidth = perimeter * card.tubeArc / closed;
    }
    if (spec.section >= 0) {
        // A section's tube: an edge loop for each turn of its curve (one every 30 degrees it
        // turns, measured on the curve at a quarter of its stations: the weave's turns, not the
        // wobble between neighbouring stations), however many `segments` asks of a card, so the
        // mesh follows the weave.
        const std::vector<Vec3f> coarse = resampleByArcLength(center.data(), center.size(), std::max<std::size_t>(8, refStations / 4));
        float turning = 0.0f;
        for (std::size_t k = 1; k + 1 < coarse.size(); ++k) {
            const Vec3f a = (coarse[k] - coarse[k - 1]).normalized(), b = (coarse[k + 1] - coarse[k]).normalized();
            turning += pm::acos(std::clamp(a.dot(b), -1.0f, 1.0f));
        }
        const int loops = static_cast<int>(std::ceil(turning / 0.524f));
        // (At most eight times `segments`: the level of detail's say, Low's 4 a third of Hero's.)
        const int most = std::min(8 * std::max(settings.segments, 1), static_cast<int>(refStations) - 1);
        card.stations = std::clamp(loops, std::min(std::max(settings.segments, 1), most), most) + 1;
    }
    sampleStations(card);
    return card;
}

// A braid's strands from where they are woven on (the Braid node's braidWoven at a half or more;
// the whole strand without the channel): what its tubes are made of. A fed strand's way in, lying
// along the scalp from its root to where it joins, would pull a tube over the scalp. In the
// order of `members`.
StrandSet wovenParts(const StrandSet& s, const std::vector<std::size_t>& members) {
    StrandSet out;
    const std::vector<float>* woven = s.pointChannel("braidWoven");
    const bool widths = !s.widths().empty();
    for (const std::size_t i : members) {
        const std::size_t n = s.strandPointCount(i), begin = s.strandBegin(i);
        std::size_t from = 0;
        if (woven) {
            while (from < n && (*woven)[begin + from] < 0.5f) ++from;
            if (from >= n) from = 0;  // never woven: all of it
            from = std::min(from, n >= 2 ? n - 2 : 0);
        }
        const Vec3f* pts = s.strandPoints(i);
        std::vector<Vec3f> points(pts + from, pts + n);
        std::vector<float> w;
        if (widths) w.assign(s.widths().begin() + static_cast<std::ptrdiff_t>(begin + from), s.widths().begin() + static_cast<std::ptrdiff_t>(begin + n));
        out.addStrand(s.ids()[i], points, w);
    }
    return out;
}

// A braid's strands from their roots to where they are woven on (braidWoven at a half), and one
// point on into the braid, so a card of them meets it: their way in. Strands with no way in (none
// under a half past the root, or no channel) get none: `has` says which made one. In the order of
// `members` (those with one).
StrandSet wayInParts(const StrandSet& s, const std::vector<std::size_t>& members, std::vector<char>* has = nullptr) {
    StrandSet out;
    const std::vector<float>* woven = s.pointChannel("braidWoven");
    const bool widths = !s.widths().empty();
    if (has) has->assign(members.size(), 0);
    if (!woven) return out;
    for (std::size_t m = 0; m < members.size(); ++m) {
        const std::size_t i = members[m];
        const std::size_t n = s.strandPointCount(i), begin = s.strandBegin(i);
        std::size_t to = 0;
        while (to < n && (*woven)[begin + to] < 0.5f) ++to;
        if (to < 2 || to >= n) continue;  // none (woven from the root, or never)
        to = std::min(to + 1, n - 1);
        const Vec3f* pts = s.strandPoints(i);
        std::vector<Vec3f> points(pts, pts + to + 1);
        std::vector<float> w;
        if (widths) w.assign(s.widths().begin() + static_cast<std::ptrdiff_t>(begin), s.widths().begin() + static_cast<std::ptrdiff_t>(begin + to + 1));
        out.addStrand(s.ids()[i], points, w);
        if (has) (*has)[m] = 1;
    }
    return out;
}

// A braid section's center line: through its strands where they are, not where their share of
// their length puts them (a section's strands differ a little in length, and fed ones join it at
// different places, which would blur the weave). The longest strand first, then each station
// pulled to the mean of the strands' points nearest it, a few times (onto the middle of the
// section, or of the whole braid for a braid's tube); as many stations as the weave needs (one
// about every half its thickness).
std::vector<Vec3f> sectionCurve(const StrandSet& s, const std::vector<std::size_t>& members) {
    float longest = 0.0f;
    for (const std::size_t i : members) longest = std::max(longest, arcLength(s.strandPoints(i), s.strandPointCount(i)));
    if (longest <= 1e-9f || members.empty()) return medianCurve(s, members);
    // Each strand at distances from its root (its points, finely).
    std::vector<std::vector<Vec3f>> fine(members.size());
    for (std::size_t m = 0; m < members.size(); ++m) {
        const std::size_t i = members[m];
        const float length = arcLength(s.strandPoints(i), s.strandPointCount(i));
        const std::size_t n = std::clamp<std::size_t>(static_cast<std::size_t>(512.0f * length / longest), 2, 512);
        fine[m] = resampleByArcLength(s.strandPoints(i), s.strandPointCount(i), n);
    }
    std::size_t longestStrand = 0;
    for (std::size_t m = 1; m < members.size(); ++m) {
        if (fine[m].size() > fine[longestStrand].size()) longestStrand = m;
    }
    auto medianAt = [&](std::size_t stations) {
        return resampleByArcLength(fine[longestStrand].data(), fine[longestStrand].size(), stations);
    };
    // How thick the section is: its strands' spread about a coarse center line.
    std::vector<Vec3f> coarse = medianAt(65);
    if (coarse.size() < 4) return medianCurve(s, members);
    std::vector<float> spread;
    for (std::size_t m = 0; m < members.size(); m += std::max<std::size_t>(1, members.size() / 64)) {
        std::size_t k = nearestStation(coarse, fine[m].front());
        for (const Vec3f& p : fine[m]) {
            k = nearestStationAhead(coarse, p, k, 4);
            spread.push_back((p - coarse[k]).length());
        }
    }
    const float thick = std::max(percentile(spread, 0.5f), 1e-6f);
    const std::size_t stations = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(longest / (0.5f * thick))) + 1, 33, 257);
    std::vector<Vec3f> center = medianAt(stations);
    if (center.size() < 4) return medianCurve(s, members);
    for (int pass = 0; pass < 5; ++pass) {
        std::vector<Vec3f> sum(center.size(), Vec3f{});
        std::vector<float> count(center.size(), 0.0f);
        for (const std::vector<Vec3f>& f : fine) {
            std::size_t k = nearestStation(center, f.front());
            for (const Vec3f& p : f) {
                k = nearestStationAhead(center, p, k, std::max<std::size_t>(4, center.size() / 16));
                sum[k] += p, count[k] += 1.0f;
            }
        }
        for (std::size_t k = 0; k < center.size(); ++k) {
            if (count[k] > 0.0f) center[k] = sum[k] * (1.0f / count[k]);
        }
        std::vector<Vec3f> smooth = center;
        for (std::size_t k = 1; k + 1 < center.size(); ++k) smooth[k] = center[k - 1] * 0.25f + center[k] * 0.5f + center[k + 1] * 0.25f;
        center = resampleByArcLength(smooth.data(), smooth.size(), smooth.size());
    }
    // Out to the strands' ends: the mean of the points nearest an end station lies inside the
    // ends (none lie past them), so each pass drew the ends in. The line runs on along its end
    // ways to the farthest root and tip.
    const Vec3f rootWay = (center[0] - center[1]).normalized(), tipWay = (center.back() - center[center.size() - 2]).normalized();
    float before = 0.0f, after = 0.0f;
    for (const std::vector<Vec3f>& f : fine) {
        before = std::max(before, (f.front() - center.front()).dot(rootWay));
        after = std::max(after, (f.back() - center.back()).dot(tipWay));
    }
    if (before > 0.0f || after > 0.0f) {
        std::vector<Vec3f> longer;
        longer.reserve(center.size() + 2);
        if (before > 0.0f) longer.push_back(center.front() + rootWay * before);
        longer.insert(longer.end(), center.begin(), center.end());
        if (after > 0.0f) longer.push_back(center.back() + tipWay * after);
        center = resampleByArcLength(longer.data(), longer.size(), center.size());
    }
    return center;
}

// The cards of one group of strands (one, or a cross), built at kRefStations: their shape,
// frame, width and texture don't depend on `segments`, which only picks how many of the
// stations become the mesh's edge loops. `strays`, when given, receives the members that
// leave the group (a flyaway, a strand the clump let escape): a card as wide as the
// farthest of its strands is a wing off the clump, so those are left out and the
// card is built from the rest (they get flyaway cards of their own instead).
std::vector<CardMesh::Card> buildGroupCards(const StrandSet& s, const std::vector<std::size_t>& members, const ScalpMesh* scalp,
                                            const CardSettings& settings, CardSpec spec,
                                            std::vector<std::size_t>* strays = nullptr) {
    std::vector<CardMesh::Card> out;
    if (members.empty()) return out;
    const bool hasScalp = scalp && scalp->triangleCount() > 0;
    if (spec.feed) {
        // A braid's feed card: made of its strands' way in (flat, along the scalp: a braid's one
        // card), baked from that part alone (card_bake).
        std::vector<char> has;
        const StrandSet way = wayInParts(s, members, &has);
        std::vector<std::size_t> all, kept;
        for (std::size_t m = 0; m < members.size(); ++m) {
            if (has[m]) all.push_back(all.size()), kept.push_back(members[m]);
        }
        if (kept.empty()) return out;
        CardSpec flat = spec;
        flat.feed = false;
        flat.cards = 1;
        flat.crossing = 1;
        flat.turn = 0;
        for (CardMesh::Card& card : buildGroupCards(way, all, scalp, settings, flat)) {
            card.strands = kept;
            card.braidFeed = true;
            out.push_back(std::move(card));
        }
        return out;
    }
    if (spec.tubeArc > 0.0f) {
        // A braid's tube: made of its strands' woven parts, along their center line.
        const StrandSet woven = wovenParts(s, members);
        std::vector<std::size_t> all(members.size());
        for (std::size_t m = 0; m < members.size(); ++m) all[m] = m;
        CardMesh::Card card = buildTubeCard(woven, all, scalp, settings, spec, sectionCurve(woven, all));
        card.strands = members;
        out.push_back(std::move(card));
        return out;
    }
    std::vector<Vec3f> center = spec.spine ? resampleSmoothCurve(spec.spine->data(), spec.spine->size(), kRefStations)
                                           : medianCurve(s, members);
    if (spec.braid >= 0 && !spec.spine && s.pointChannel("braidWoven")) {
        // A braid's flat cards run along the braid where it is woven: the strands' share of their
        // length lines up a braid's hair, but not a fed one's (rooted all over the scalp, joining
        // along the way), whose median cut through the head.
        const StrandSet woven = wovenParts(s, members);
        std::vector<std::size_t> all(members.size());
        for (std::size_t m = 0; m < members.size(); ++m) all[m] = m;
        const std::vector<Vec3f> along = sectionCurve(woven, all);
        if (along.size() >= 2) center = resampleByArcLength(along.data(), along.size(), kRefStations);
    }
    if (spec.spine) {
        // Cards from guides follow the guide's shape, but Clump's past-tip Extend can preserve
        // strands beyond that guide. Keep following the guide's final direction until the card
        // has room for the longest member's true arc length.
        const float spineLength = arcLength(center);
        float longest = 0.0f;
        for (const std::size_t i : members) {
            longest = std::max(longest, arcLength(s.strandPoints(i), s.strandPointCount(i)));
        }
        if (spineLength > 1e-9f && longest > spineLength) {
            const Vec3f tip = (center.back() - center[center.size() - 2]).normalized();
            std::vector<Vec3f> extended = center;
            extended.push_back(center.back() + tip * (longest - spineLength + 0.01f * spineLength));
            center = resampleByArcLength(extended.data(), extended.size(), kRefStations);
        }
    }
    const std::vector<Frame> frames = cardFrames(center, hasScalp ? scalp : nullptr);
    const std::vector<Offset> offsets = strandOffsets(s, members, center, 16);
    const float twist = settings.twist * 0.0174532925f;
    const float cardLength = std::max(arcLength(center), 1e-9f);

    // Strays: a member that gets much farther from the group's centre line than the
    // group does (2.5x the median of the members' farthest offsets, plus a few strand
    // widths) leaves, when enough remain for a card. A flyaway card (loose strands rooted
    // beside each other, which may well head different ways) is stricter: a member that
    // leaves its centre line by more than a share of the card's length goes, however few
    // remain, and is dropped (a lone wild strand is not worth a card): the card stays the
    // thin thing it is meant to be rather than a sheet spanning every direction.
    if ((strays && members.size() >= 4) || (spec.flyaway && members.size() >= 2)) {
        float widthGuess = 0.005f * cardLength;
        if (!s.widths().empty()) {
            float sum = 0.0f, n = 0.0f;
            for (const std::size_t i : members) {
                for (std::size_t q = 0; q < s.strandPointCount(i); ++q) sum += s.widths()[s.strandBegin(i) + q], n += 1.0f;
            }
            if (n > 0.0f && sum / n != 1.0f) widthGuess = sum / n;
        }
        std::vector<float> farthest(members.size(), 0.0f);
        for (std::size_t m = 0; m < members.size(); ++m) {
            for (std::size_t j = 0; j < 16; ++j) farthest[m] = std::max(farthest[m], offsets[m * 16 + j].d.length());
        }
        std::vector<float> sorted = farthest;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        float limit = 2.5f * sorted[sorted.size() / 2] + 3.0f * widthGuess;
        if (spec.flyaway) limit = std::min(limit, std::max(0.12f * cardLength, 3.0f * widthGuess));
        std::vector<std::size_t> kept, left;
        for (std::size_t m = 0; m < members.size(); ++m) (farthest[m] > limit ? left : kept).push_back(members[m]);
        if (!left.empty() && spec.flyaway) {
            // The farthest goes; the rest measure again without it (its pull on the centre line gone).
            std::vector<std::size_t> without = members;
            const auto worst = static_cast<std::size_t>(std::max_element(farthest.begin(), farthest.end()) - farthest.begin());
            without.erase(without.begin() + static_cast<std::ptrdiff_t>(worst));
            if (strays) strays->push_back(members[worst]);
            return buildGroupCards(s, without, scalp, settings, spec, strays);
        }
        if (!left.empty() && static_cast<int>(kept.size()) >= std::max(settings.minStrands, 1)) {
            strays->insert(strays->end(), left.begin(), left.end());
            return buildGroupCards(s, kept, scalp, settings, spec, nullptr);
        }
    }

    // The way the strands lie across the card: their principal direction in the cross-section
    // at every station, reduced to one weighted card-wide angle. Fit Twist rotates the card as a
    // whole; it never introduces a root-to-tip turn.
    float fitAngle = 0.0f;
    if (settings.fitTwist > 0.0f) {
        std::vector<float> suu(kRefStations, 0.0f), svv(kRefStations, 0.0f), suv(kRefStations, 0.0f);
        for (const Offset& o : offsets) {
            for (std::size_t k = o.station > 1 ? o.station - 2 : 0; k <= std::min(o.station + 2, kRefStations - 1); ++k) {
                const float u = o.d.dot(frames[k].binormal), v = o.d.dot(frames[k].normal);
                suu[k] += u * u, svv[k] += v * v, suv[k] += u * v;
            }
        }
        // Angles unwrapped against the last station that had a clear axis (an axis
        // repeats every pi), each weighted by how clear the axis is.
        double sw = 0.0, sa = 0.0;
        float previous = 0.0f;
        for (std::size_t k = 0; k < kRefStations; ++k) {
            const float total = suu[k] + svv[k];
            const float anisotropy = total > 1e-20f ? std::sqrt((suu[k] - svv[k]) * (suu[k] - svv[k]) + 4.0f * suv[k] * suv[k]) / total : 0.0f;
            const float w = std::clamp((anisotropy - 0.15f) / 0.5f, 0.0f, 1.0f);
            if (w <= 0.0f) continue;
            float angle = 0.5f * pm::atan2(2.0f * suv[k], suu[k] - svv[k]);
            while (angle - previous > 1.5707963f) angle -= 3.14159265f;
            while (angle - previous < -1.5707963f) angle += 3.14159265f;
            previous = angle;
            sw += w;
            sa += w * angle;
        }
        if (sw > 0.5) {
            fitAngle = static_cast<float>(sa / sw) * std::clamp(settings.fitTwist, 0.0f, 1.0f);
        }
    }

    // How many cards: one, a cross of crossCount, or (auto) a cross for round tufts standing
    // off the scalp (a single ribbon shows them as a flat blade).
    const int crossCount = std::clamp(settings.crossCount, 2, 6);
    int crossing = settings.crossStyle == CardSettings::CrossStyle::Single  ? 1
                   : settings.crossStyle == CardSettings::CrossStyle::Cross ? crossCount
                                                                            : 0;
    if (spec.flyaway) crossing = 1;
    if (spec.braid >= 0) crossing = std::clamp(spec.cards, 1, 6);
    if (spec.crossing > 0) crossing = spec.crossing;  // a rebuild: as it was
    if (crossing == 0) {
        const std::size_t mid = kRefStations / 2;
        float sx = 0.0f, sy = 0.0f;
        for (const Offset& o : offsets) {
            if (o.station < mid - 2 || o.station > mid + 2) continue;
            sx += o.d.dot(frames[mid].binormal) * o.d.dot(frames[mid].binormal);
            sy += o.d.dot(frames[mid].normal) * o.d.dot(frames[mid].normal);
        }
        const float round = std::sqrt(std::min(sx, sy) / std::max(std::max(sx, sy), 1e-20f));
        const Vec3f rise = (center.back() - center.front()).normalized();
        const Vec3f skin = hasScalp ? scalp->closestPoint(center.front()).normal.normalized() : Vec3f{0, 1, 0};
        crossing = round > 0.45f && std::abs(rise.dot(skin)) > 0.5f ? crossCount : 1;
    }
    // Rebuilding one card (strands attached): only that one of its cross.
    const int from = spec.crossing > 0 ? spec.turn : 0, to = spec.crossing > 0 ? spec.turn + 1 : crossing;
    // Crossed cards stand for a round tuft: its strands have no axis to follow, so
    // the cross keeps its set facings instead of turning with a noisy fit.
    if (crossing > 1 || spec.braid >= 0) fitAngle = 0.0f;

    // The members' widths, for cards of one or two strands (no spread to measure).
    float strandWidth = 0.0f;
    if (!s.widths().empty()) {
        float sum = 0.0f, n = 0.0f;
        for (const std::size_t i : members) {
            for (std::size_t p = 0; p < s.strandPointCount(i); ++p) sum += s.widths()[s.strandBegin(i) + p], n += 1.0f;
        }
        strandWidth = n > 0.0f ? sum / n : 0.0f;
        if (strandWidth == 1.0f) strandWidth = 0.0f;  // the default: not real widths
    }
    if (strandWidth <= 0.0f) strandWidth = 0.005f * cardLength;

    // Where the members grow: the root nearest their mean (the mean of roots spread over a
    // curved scalp lies under it).
    const auto fromMean = [&](const Vec3f& r) { return (r - center.front()).dot(r - center.front()); };
    Vec3f rootNearest = s.strandPoints(members.front())[0];
    for (const std::size_t i : members) {
        if (fromMean(s.strandPoints(i)[0]) < fromMean(rootNearest)) rootNearest = s.strandPoints(i)[0];
    }

    const float ruffleSign = spec.parity % 2 == 0 ? 1.0f : -1.0f;
    const float ruffle = spec.ruffled && !spec.flyaway ? std::clamp(settings.ruffle, 0.0f, 1.0f) : 0.0f;
    for (int j = from; j < to; ++j) {
        CardMesh::Card card;
        card.strands = members;
        card.clump = spec.clump;
        card.layer = spec.layer;
        card.flyaway = spec.flyaway;
        card.braid = spec.braid;
        // A textured line cannot look smoother than the ribbon it is mapped onto. Keep hair
        // cards at the reference curve's resolution so the bake stays clean instead of encoding
        // the visible corners of a coarse eight-quad strip into its atlas.
        card.stations = std::max(settings.segments, 1) + 1;
        card.across = std::clamp(settings.widthSegments, 1, 16);
        card.curvature = std::clamp(settings.curvature, 0.0f, 1.0f);
        card.refCenter = center;
        const float turn = 3.14159265f * static_cast<float>(j) / static_cast<float>(std::max(crossing, 1)) +
                           ruffleSign * ruffle * 0.26f * (crossing > 1 ? 0.0f : 1.0f);
        for (std::size_t k = 0; k < kRefStations; ++k) {
            const Frame& f = frames[k];
            // The fit turns the card the way its strands lie; twist turns it around its
            // length on top; crossed cards turn further.
            const float a = turn - fitAngle;
            card.refNormal.push_back(f.normal * pm::cos(a) + f.binormal * pm::sin(a));
            card.refSide.push_back(f.binormal * pm::cos(a) - f.normal * pm::sin(a));
        }
        // Flatten toward a developable ribbon. Merely projecting one mean direction onto every
        // station tangent still leaves successive sides out of the plane of their shared
        // centerline segment; Maya triangulates that non-planar quad and its two UV interpolants
        // show as a wave. Propagate each next side in the plane of the previous side and the
        // segment, while keeping it perpendicular to the next station tangent. At flatten 1 every
        // unbowed quad is therefore planar. Fit twist chooses the initial facing of the ribbon.
        const float flatten = std::clamp(settings.flatten, 0.0f, 1.0f);
        if (flatten > 0.0f) {
            Vec3f mean{};
            for (const Vec3f& side : card.refSide) mean += side;
            if (mean.length() > 1e-6f) {
                mean = mean.normalized();
                std::vector<Vec3f> tangent(kRefStations);
                for (std::size_t k = 0; k < kRefStations; ++k) {
                    const std::size_t before = k > 0 ? k - 1 : 0;
                    const std::size_t after = std::min(k + 1, kRefStations - 1);
                    tangent[k] = (center[after] - center[before]).normalized();
                }
                std::vector<Vec3f> flat(kRefStations);
                flat[0] = mean - tangent[0] * mean.dot(tangent[0]);
                if (flat[0].length() <= 1e-6f) flat[0] = card.refSide[0];
                flat[0] = flat[0].normalized();
                if (flat[0].dot(card.refSide[0]) < 0.0f) flat[0] = flat[0] * -1.0f;
                for (std::size_t k = 0; k + 1 < kRefStations; ++k) {
                    const Vec3f segment = (center[k + 1] - center[k]).normalized();
                    const Vec3f planeNormal = flat[k].cross(segment).normalized();
                    Vec3f next = tangent[k + 1].cross(planeNormal);
                    if (next.length() <= 1e-6f) next = flat[k];
                    next = next.normalized();
                    if (next.dot(flat[k]) < 0.0f) next = next * -1.0f;
                    flat[k + 1] = next;
                }
                for (std::size_t k = 0; k < kRefStations; ++k) {
                    Vec3f side = card.refSide[k] * (1.0f - flatten) + flat[k] * flatten;
                    side = side - tangent[k] * side.dot(tangent[k]);
                    if (side.length() <= 1e-6f) continue;
                    card.refSide[k] = side.normalized();
                    card.refNormal[k] = card.refSide[k].cross(tangent[k]).normalized();
                }
            }
        }
        // A braid's one card (a cornrow's, lying on the scalp): it faces out of the skin under
        // each station, whichever way a frame carried along a wavering center line would turn.
        if (spec.braid >= 0 && crossing == 1 && hasScalp) {
            for (std::size_t k = 0; k < kRefStations; ++k) {
                const Vec3f tangent = (center[std::min(k + 1, kRefStations - 1)] - center[k > 0 ? k - 1 : 0]).normalized();
                Vec3f up = scalp->closestPoint(center[k]).normal;
                up = up - tangent * up.dot(tangent);
                if (up.length() < 1e-4f) continue;
                card.refNormal[k] = up.normalized();
                card.refSide[k] = tangent.cross(card.refNormal[k]).normalized();
            }
        }
        // Authored Twist stays on top of the fitted/flattened card. Flatten is a remedy for the
        // fit's noisy local rotation, not a way to disable an intentional twist control.
        if (std::abs(twist) > 1e-9f) {
            for (std::size_t k = 0; k < kRefStations; ++k) {
                const float along = static_cast<float>(k) / static_cast<float>(kRefStations - 1);
                const float a = twist * along;
                const Vec3f normal = card.refNormal[k], side = card.refSide[k];
                card.refNormal[k] = normal * pm::cos(a) + side * pm::sin(a);
                card.refSide[k] = side * pm::cos(a) - normal * pm::sin(a);
            }
        }
        // Width: every retained member strand, each by its farthest point across it in its plane
        // here and at the neighbouring stations, fits across the card with a margin each side.
        // True flyaways have already been split onto flyaway cards; clipping the remaining outer
        // members to a percentile makes their bake run along the cell edge and visibly cut off.
        std::vector<float> spread(kRefStations, 0.0f);
        {
            std::vector<std::vector<float>> across(kRefStations);
            // A point past either end (a longer strand running on past the card's tip, measured
            // from the end station) is no measure of how wide the card is there: its way along
            // would read as across and flare the tip.
            const Vec3f startDir = (center[1] - center[0]).normalized();
            const Vec3f endDir = (center[kRefStations - 1] - center[kRefStations - 2]).normalized();
            // Each strand counts once at a station (its farthest point there): one strand drifting
            // off the group is one strand of many, however many of its points land there.
            const std::size_t samples = offsets.size() / members.size();
            std::vector<float> farthest(kRefStations);
            for (std::size_t m = 0; m < members.size(); ++m) {
                std::fill(farthest.begin(), farthest.end(), -1.0f);
                for (std::size_t j = 0; j < samples; ++j) {
                    const Offset& o = offsets[m * samples + j];
                    if ((o.station == kRefStations - 1 && o.d.dot(endDir) > 0.0f) || (o.station == 0 && o.d.dot(startDir) < 0.0f)) continue;
                    const float x = std::abs(o.d.dot(card.refSide[o.station]));
                    for (std::size_t k = o.station > 0 ? o.station - 1 : 0; k <= std::min(o.station + 1, kRefStations - 1); ++k) {
                        farthest[k] = std::max(farthest[k], x);
                    }
                }
                for (std::size_t k = 0; k < kRefStations; ++k) {
                    if (farthest[k] >= 0.0f) across[k].push_back(farthest[k]);
                }
            }
            for (std::size_t k = 0; k < kRefStations; ++k) {
                if (across[k].empty()) continue;
                spread[k] = *std::max_element(across[k].begin(), across[k].end());
            }
        }
        // Smoothed up the card, never below the spread there: no notches between wider
        // stations (a dip narrower than a fifth of the card fills to the lower of the
        // flanks), and every strand still fits.
        {
            std::vector<float> filled = spread;
            constexpr std::size_t kReach = 3;
            for (std::size_t k = 0; k < kRefStations; ++k) {
                float before = 0.0f, after = 0.0f;
                for (std::size_t j = k > kReach ? k - kReach : 0; j <= k; ++j) before = std::max(before, spread[j]);
                for (std::size_t j = k; j <= std::min(k + kReach, kRefStations - 1); ++j) after = std::max(after, spread[j]);
                filled[k] = std::max(spread[k], std::min(before, after));
            }
            std::vector<float> smooth = filled;
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<float> next = smooth;
                for (std::size_t k = 1; k + 1 < kRefStations; ++k) next[k] = 0.25f * smooth[k - 1] + 0.5f * smooth[k] + 0.25f * smooth[k + 1];
                smooth = next;
            }
            for (std::size_t k = 0; k < kRefStations; ++k) spread[k] = std::max(spread[k], smooth[k]);
        }
        // Tips: hair doesn't get wider toward its ends, so the last quarter of the card is
        // never wider than the card was before it (one strand's tip flaring out pokes out of
        // the card rather than blowing it up); and the card narrows no faster than a
        // quarter of its width per station, so where the strands end (the card runs on to
        // the farthest tip, with nothing across it there) it tapers instead of pinching to
        // nothing in one step.
        {
            const std::size_t tipFrom = 3 * kRefStations / 4;
            float widest = 0.0f;
            for (std::size_t k = 0; k <= tipFrom; ++k) widest = std::max(widest, spread[k]);
            // (Nor wider than the station before it: the few strands reaching the end, off its
            // middle, would flare it back out to the root's width.)
            for (std::size_t k = tipFrom + 1; k < kRefStations; ++k) spread[k] = std::min({spread[k], widest, spread[k - 1]});
            const float maxDrop = 4.0f * widest / static_cast<float>(kRefStations);  // a full taper takes a quarter of the card
            for (std::size_t k = 1; k < kRefStations; ++k) spread[k] = std::max(spread[k], spread[k - 1] - maxDrop);
        }
        const float margin = std::clamp(settings.margin, 0.0f, 0.45f);
        const float widthFloor = members.size() < 3 ? 4.0f * strandWidth : 2.0f * strandWidth;
        for (std::size_t k = 0; k < kRefStations; ++k) {
            spread[k] = std::max(2.0f * spread[k] + 2.0f * strandWidth, widthFloor) / (1.0f - 2.0f * margin);
            card.refWidth.push_back(std::max(spread[k] * settings.widthScale, settings.minWidth));
        }
        // Hair cards use a rectangular UV grid. The widest required station sets the width
        // for every row. Apply the authored profile afterward so its endpoint can still control
        // the tip; the default constant profile remains a non-tapered rectangle.
        const float uniformWidth = *std::max_element(card.refWidth.begin(), card.refWidth.end());
        for (std::size_t k = 0; k < kRefStations; ++k) {
            const float along = static_cast<float>(k) / static_cast<float>(kRefStations - 1);
            card.refWidth[k] = std::max(uniformWidth * std::max(settings.widthProfile(along), 0.0f), settings.minWidth);
        }
        // The texture's frame: one scale along the whole card, the widest station's.
        card.textureWidth = *std::max_element(card.refWidth.begin(), card.refWidth.end());
        // Ruffle: alternate cards push out or in along their normal, a share of their width, but
        // never farther than their strands lie off it (the hair's own thickness there): a card
        // pushed past its strands puffs the groom up.
        if (ruffle > 0.0f) {
            float meanSpread = 0.0f;
            for (const float w : spread) meanSpread += w / static_cast<float>(kRefStations);
            std::vector<float> depths;
            depths.reserve(offsets.size());
            for (const Offset& o : offsets) depths.push_back(std::abs(o.d.dot(card.refNormal[o.station])));
            const float push = std::min(ruffle * 0.15f * meanSpread, percentile(depths, kFitShare));
            for (std::size_t k = 0; k < kRefStations; ++k) card.refCenter[k] += card.refNormal[k] * (ruffleSign * push);
        }
        // Out to its strands' outer side: a card through their middle leaves the outer half of
        // their thickness uncovered, and the groom's silhouette (the outermost strands) shrinks
        // by it, most where the cards are seen edge-on. A ribbon moves out to where a quarter of
        // its strands lie farther out (away from the scalp), never past its strands.
        // Which way out of the scalp the card's normal faces (+1: away from it).
        float facing = 1.0f;
        if (hasScalp) {
            const std::size_t mid = kRefStations / 2;
            const SurfacePoint near = scalp->closestPoint(card.refCenter[mid]);
            Vec3f away = card.refCenter[mid] - near.position;
            if (away.length() < 1e-6f) away = near.normal;
            facing = away.dot(card.refNormal[mid]) >= 0.0f ? 1.0f : -1.0f;
        }
        // (A guide's card stays on its guide.)
        if (hasScalp && crossing == 1 && spec.braid < 0 && !spec.flyaway && !spec.spine && offsets.size() >= 8) {
            std::vector<float> depths;
            depths.reserve(offsets.size());
            for (const Offset& o : offsets) depths.push_back(facing * o.d.dot(card.refNormal[o.station]));
            const float shell = std::max(percentile(depths, kOuterShell), 0.0f);
            for (std::size_t k = 0; k < kRefStations; ++k) card.refCenter[k] += card.refNormal[k] * (facing * shell);
        }
        // Clear of the scalp: a card lying on the skin (roots, hair combed flat) sits a hair
        // above it, so the scalp doesn't cut through or shade it. Measured at the card's
        // center line, then the pushes are smoothed along the card: a wide card on a curved
        // head has its edges dip below the surface by the curvature, and lifting each
        // station by its own edges' dip kinked the card at the root.
        if (hasScalp) {
            const float clearance = 0.01f * std::max(arcLength(card.refCenter), 1e-6f);
            std::vector<float> push(kRefStations, 0.0f);
            std::vector<Vec3f> skin(kRefStations, Vec3f{0, 1, 0});
            for (std::size_t k = 0; k < kRefStations; ++k) {
                const SurfacePoint near = scalp->closestPoint(card.refCenter[k], 4.0f * clearance);
                if (near.root.face < 0) continue;
                skin[k] = near.normal.normalized();
                const float h = (card.refCenter[k] - near.position).dot(skin[k]);
                if (h < clearance) push[k] = clearance - h;
            }
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<float> smooth = push;
                for (std::size_t k = 1; k + 1 < kRefStations; ++k) smooth[k] = 0.25f * push[k - 1] + 0.5f * push[k] + 0.25f * push[k + 1];
                push = smooth;
            }
            for (std::size_t k = 0; k < kRefStations; ++k) {
                if (push[k] > 0.0f) card.refCenter[k] += skin[k] * push[k];
            }
        }
        if (settings.volumeNormals && hasScalp) {
            // Shaded as the strands it stands for are: a strand's lit drawing leans its normal
            // out of the scalp at its root, half its part along the strand taken out
            // (display.cpp's strandFrame), so the card takes its members' root normal leaned the
            // same way along its own direction, station by station. Combed hair then shades like
            // the skin it grows from, as its strands do, hair hanging below the head too.
            const Vec3f up = cardRootNormal(s, members, *scalp, card.refNormal.front());
            for (std::size_t k = 0; k < kRefStations; ++k) {
                card.refShading.push_back(strandLookNormal(up, card.refCenter, k));
            }
        }
        sampleStations(card);
        // The base on the scalp under the card: the stations' root row is a straight line across
        // (bowed by the curvature, tilted by the fit), at the members' mean root lifted clear of
        // the skin, and over a curved head a wide card's floated well above the roots it holds.
        // Fit Twist rotates the card as one ribbon, including its freely turning root row. Blend
        // from per-vertex scalp seating to one root-anchored rigid translation by the same amount;
        // at full fit the first quad keeps exactly the card's orientation.
        if (hasScalp) seatCard(card, *scalp, rootNearest, strandWidth, std::clamp(settings.fitTwist, 0.0f, 1.0f));
        out.push_back(std::move(card));
    }
    return out;
}

}  // namespace

std::size_t CardMesh::textureCount() const {
    return static_cast<std::size_t>(std::count_if(cards.begin(), cards.end(), [](const Card& c) { return c.variationOf < 0; }));
}

std::vector<CardMesh::Card> makeCards(const StrandSet& s, const ScalpMesh* scalp, const CardSettings& settings) {
    std::vector<CardMesh::Card> out;
    if (s.empty()) return out;
    const int minStrands = std::max(settings.minStrands, 1);
    const ScalpMesh* skin = scalp && scalp->triangleCount() > 0 ? scalp : nullptr;

    // Feathers: carded per feather, apart from the hair, and taking their cards from the
    // budget first; `feathered` marks the strands the hair's cards leave alone (the feathers,
    // and with PerFeather everything; empty: none).
    std::vector<char> feathered;
    std::vector<CardMesh::Card> featherCards;
    {
        using Mode = CardSettings::FeatherMode;
        const Mode mode = settings.featherMode;
        if (mode != Mode::PerVane && (mode == Mode::PerFeather || s.strandChannel(kFeatherIdChannel))) {
            feathered.assign(s.strandCount(), 0);
            if (mode == Mode::Off) {
                groupFeathers(s, feathered);  // only marks them
            } else {
                featherCards = makeFeatherCards(s, skin, settings, feathered);
            }
            if (mode == Mode::PerFeather) std::fill(feathered.begin(), feathered.end(), 1);
            if (std::none_of(feathered.begin(), feathered.end(), [](char c) { return c != 0; })) feathered.clear();
        }
    }
    // The hair's share of the budget.
    const int hairMax = settings.maxCards > 0 ? std::max(settings.maxCards - static_cast<int>(featherCards.size()), 1) : 0;
    std::vector<int> cluster = clusterStrands(s, settings, feathered);

    // Braids: the strands a Braid node wove (its braidId channel) leave the clumps and make
    // one card set per braid, built first and taking their cards from the budget first: a
    // cross of crossCount cards along the braid (the woven strands' median curve runs down its
    // axis), or one ribbon lying on the scalp for a cornrow (braidFlat 1). Strands the Braid
    // left loose (braidId -1) and braids too small for a card go the usual way.
    std::vector<char> braided = feathered.empty() ? std::vector<char>(s.strandCount(), 0) : feathered;
    std::vector<CardSpec> specs;
    int groups = 0;
    {
        // (Per section too, for section tubes: the key the braid's id times 64 plus its section + 1.)
        const bool bySection = settings.braidCards == CardSettings::BraidCards::Sections && s.strandChannel("braidSection");
        const std::vector<float>* sectionOf = s.strandChannel("braidSection");
        std::map<long long, std::vector<std::size_t>> byBraid;
        if (settings.braids) {
            if (const std::vector<float>* ids = s.strandChannel("braidId")) {
                for (std::size_t i = 0; i < s.strandCount(); ++i) {
                    long long key = std::llround((*ids)[i]);
                    if (key >= 0 && bySection) key = key * 64 + std::clamp<long long>(std::llround((*sectionOf)[i]), -1, 62) + 1;
                    if (key < 0 || braided[i] || s.strandPointCount(i) < 2 ||
                        arcLength(s.strandPoints(i), s.strandPointCount(i)) <= 0.0f) {
                        continue;
                    }
                    byBraid[key].push_back(i);
                }
            }
        }
        std::vector<std::pair<long long, std::vector<std::size_t>>> braids;
        for (auto& [id, members] : byBraid) {
            if (static_cast<int>(members.size()) < minStrands) continue;
            for (const std::size_t i : members) braided[i] = 1, cluster[i] = -1;
            braids.emplace_back(id, std::move(members));
        }
        if (!braids.empty()) {  // the clusters left, numbered without gaps
            std::map<int, int> renumber;
            for (int& c : cluster) {
                if (c >= 0) c = renumber.emplace(c, static_cast<int>(renumber.size())).first->second;
            }
        }
        const std::vector<float>* flat = s.strandChannel("braidFlat");
        std::vector<std::vector<CardMesh::Card>> braidCards(braids.size());
        parallelFor(0, braids.size(), [&](std::size_t b) {
            const std::vector<std::size_t>& members = braids[b].second;
            float flatness = 0.0f;
            if (flat) {
                for (const std::size_t i : members) flatness += (*flat)[i] / static_cast<float>(members.size());
            }
            CardSpec spec;
            spec.braid = static_cast<int>(bySection ? braids[b].first / 64 : braids[b].first);
            spec.cards = flatness > 0.5f ? 1 : std::max(settings.crossCount, 2);
            if (bySection) {
                // A section's tube, closed (a cornrow's sections too: they wind over each other).
                spec.cards = 1;
                spec.section = static_cast<int>(braids[b].first % 64) - 1;
                spec.tubeArc = 6.28318530718f;
            } else if (settings.braidCards != CardSettings::BraidCards::Cross) {
                // A tube: closed around a hanging braid, its upper part over a cornrow.
                spec.cards = 1;
                spec.tubeArc = flatness > 0.5f ? kCornrowArc : 6.28318530718f;
            }
            braidCards[b] = buildGroupCards(s, members, skin, settings, spec);
            if (spec.tubeArc > 0.0f && (!bySection || spec.section == 0)) {
                // The tubes stand for the braid where it is woven; the hair's way into it (fed hair
                // along the scalp, a box braid's gather) gets feed cards: its strands' way-in parts
                // grouped by where they come from, run and join (k-means), so each group's strands
                // lie together, a narrow card of them from the scalp into the braid. As many as a
                // clump's hair gets: about 120 / cardsPerClump strands a card (the level of
                // detail's say), at most 128 a braid.
                std::vector<std::size_t> whole;
                if (bySection) {
                    const long long id = braids[b].first / 64;
                    for (const auto& [key, others] : braids) {
                        if (key / 64 == id) whole.insert(whole.end(), others.begin(), others.end());
                    }
                    std::sort(whole.begin(), whole.end());
                } else {
                    whole = members;
                }
                std::vector<char> has;
                const StrandSet way = wayInParts(s, whole, &has);
                std::vector<std::size_t> fed;
                for (std::size_t m = 0; m < whole.size(); ++m) {
                    if (has[m]) fed.push_back(whole[m]);
                }
                if (static_cast<int>(fed.size()) >= std::max(settings.minStrands, 1)) {
                    std::vector<std::vector<float>> where(way.strandCount());
                    for (std::size_t w = 0; w < way.strandCount(); ++w) {
                        const std::size_t n = way.strandPointCount(w);
                        const Vec3f r = way.strandPoints(w)[0], m = way.strandPoints(w)[n / 2], j = way.strandPoints(w)[n - 1];
                        where[w] = {r.x, r.y, r.z, m.x, m.y, m.z, j.x, j.y, j.z};
                    }
                    const float perCard = std::clamp(120.0f / static_cast<float>(std::max(settings.cardsPerClump, 1)), 12.0f, 120.0f);
                    const int groups = std::clamp(static_cast<int>(std::lround(static_cast<float>(fed.size()) / perCard)), 1, 128);
                    const std::vector<int> label = kMeans(where, static_cast<std::size_t>(groups), 12);
                    std::map<int, std::vector<std::size_t>> byGroup;
                    for (std::size_t w = 0; w < fed.size(); ++w) byGroup[label[w]].push_back(fed[w]);
                    CardSpec feed;
                    feed.braid = spec.braid;
                    feed.feed = true;
                    for (auto& [g, strands] : byGroup) {
                        if (static_cast<int>(strands.size()) < std::max(settings.minStrands, 1)) continue;
                        for (CardMesh::Card& card : buildGroupCards(s, strands, skin, settings, feed)) braidCards[b].push_back(std::move(card));
                    }
                }
            }
        }, 1);
        for (std::size_t b = 0; b < braids.size(); ++b) {
            if (braidCards[b].empty()) continue;
            CardSpec spec;
            spec.braid = static_cast<int>(bySection ? braids[b].first / 64 : braids[b].first);
            spec.cards = static_cast<int>(braidCards[b].size());
            spec.crossing = spec.cards;
            spec.group = groups++;
            for (std::size_t j = 0; j < braidCards[b].size(); ++j) {
                // Each card rebuilt as it was: a tube as a tube (of its section), a cornrow's flat
                // card under its tubes as the one card it is, a cross's card as its turn.
                const CardMesh::Card& card = braidCards[b][j];
                CardSpec own = spec;
                own.tubeArc = card.tubeArc;
                own.section = card.braidSection;
                own.feed = card.braidFeed;
                own.turn = static_cast<int>(j);
                if (spec.cards > 1 && (card.tubeArc > 0.0f || braidCards[b].front().tubeArc > 0.0f)) {
                    own.cards = 1, own.crossing = 1, own.turn = 0;
                }
                specs.push_back(own);
                out.push_back(std::move(braidCards[b][j]));
            }
        }
    }
    // Cards from guides: each strand left joins the guide whose root is nearest its own, and each
    // guide holding enough of them is a card set along the guide itself; nothing is left for the
    // clumps (strands of guides holding too few make no card).
    if (!settings.guideSpines.empty()) {
        std::vector<Vec3f> guideRoots;
        std::vector<const std::vector<Vec3f>*> spines;
        for (const std::vector<Vec3f>& spine : settings.guideSpines) {
            if (spine.size() < 2 || !(arcLength(spine.data(), spine.size()) > 0.0f)) continue;
            guideRoots.push_back(spine.front());
            spines.push_back(&spine);
        }
        std::vector<std::vector<std::size_t>> byGuide(guideRoots.size());
        if (!guideRoots.empty()) {
            const PointIndex index(guideRoots);
            for (std::size_t i = 0; i < s.strandCount(); ++i) {
                if (braided[i] || s.strandPointCount(i) < 2 || arcLength(s.strandPoints(i), s.strandPointCount(i)) <= 0.0f) continue;
                const std::size_t g = index.nearest(s.strandPoints(i)[0]);
                if (g < byGuide.size()) byGuide[g].push_back(i);
            }
        }
        std::vector<std::vector<CardMesh::Card>> guideCards(byGuide.size());
        parallelFor(0, byGuide.size(), [&](std::size_t g) {
            if (static_cast<int>(byGuide[g].size()) < minStrands) return;
            CardSpec spec;
            spec.spine = spines[g];
            guideCards[g] = buildGroupCards(s, byGuide[g], skin, settings, spec);
        }, 1);
        for (std::size_t g = 0; g < guideCards.size(); ++g) {
            if (guideCards[g].empty()) continue;
            CardSpec spec;
            spec.spine = spines[g];
            spec.crossing = static_cast<int>(guideCards[g].size());
            spec.group = groups++;
            for (std::size_t j = 0; j < guideCards[g].size(); ++j) {
                spec.turn = static_cast<int>(j);
                specs.push_back(spec);
                out.push_back(std::move(guideCards[g][j]));
            }
        }
        std::fill(braided.begin(), braided.end(), 1);
        std::fill(cluster.begin(), cluster.end(), -1);
    }
    // The budget left for the clumps.
    int clumpBudget = hairMax;
    if (clumpBudget > 0) clumpBudget = std::max(clumpBudget - static_cast<int>(out.size()), 1);

    // More groups than the budget holds: neighbouring clumps merge.
    mergeClusters(s, cluster, clumpBudget, minStrands);
    int clusterCount = 0;
    for (const int c : cluster) clusterCount = std::max(clusterCount, c + 1);
    std::vector<std::vector<std::size_t>> members(static_cast<std::size_t>(clusterCount));
    std::vector<std::size_t> pool;  // strands no card holds (yet)
    // A Flyaway node's strays (its "flyaway" flag) are no part of their clump's card (they would
    // widen it): each gets a thin card of its own, first of the flyaway cards' budget.
    const std::vector<float>* flyawayFlag = s.strandChannel("flyaway");
    if (flyawayFlag && flyawayFlag->size() != s.strandCount()) flyawayFlag = nullptr;
    // Strays are a few strands leaving their clump: where most of a clump is flagged (a Flyaway
    // with a large fraction, styling the hair rather than adding strays), its strands are the
    // clump's hair and make its cards, or the hair would make no card at all.
    std::vector<float> flaggedShare(static_cast<std::size_t>(clusterCount), 0.0f);
    if (flyawayFlag) {
        std::vector<float> total(static_cast<std::size_t>(clusterCount), 0.0f);
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            if (braided[i] || cluster[i] < 0) continue;
            total[static_cast<std::size_t>(cluster[i])] += 1.0f;
            if ((*flyawayFlag)[i] > 0.5f) flaggedShare[static_cast<std::size_t>(cluster[i])] += 1.0f;
        }
        for (std::size_t c = 0; c < flaggedShare.size(); ++c) flaggedShare[c] = total[c] > 0.0f ? flaggedShare[c] / total[c] : 0.0f;
    }
    std::vector<char> isFlyaway(s.strandCount(), 0);
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (braided[i] || s.strandPointCount(i) < 2 || arcLength(s.strandPoints(i), s.strandPointCount(i)) <= 0.0f) continue;
        const bool clumpOfStrays = cluster[i] >= 0 && flaggedShare[static_cast<std::size_t>(cluster[i])] > 0.5f;
        if (flyawayFlag && (*flyawayFlag)[i] > 0.5f && !clumpOfStrays) {
            isFlyaway[i] = 1;
            pool.push_back(i);
        } else if (cluster[i] >= 0) {
            members[static_cast<std::size_t>(cluster[i])].push_back(i);
        } else {
            pool.push_back(i);
        }
    }

    // Cards per clump, within the budget; each clump's strands split into that many groups.
    std::vector<std::size_t> counts(members.size());
    for (std::size_t c = 0; c < members.size(); ++c) counts[c] = members[c].size();
    const int layers = std::clamp(settings.layers, 1, 3);
    const std::vector<int> perClump =
        allocateCards(counts, std::max(settings.cardsPerClump, 1) * layers, minStrands, clumpBudget);
    std::vector<std::vector<CardMesh::Card>> perClumpCards(members.size());
    std::vector<std::vector<std::size_t>> perClumpPool(members.size());
    std::vector<std::vector<std::size_t>> perClumpStrays(members.size());  // left out of their cards
    std::vector<std::vector<CardSpec>> perClumpSpecs(members.size());
    parallelFor(0, members.size(), [&](std::size_t c) {
        if (perClump[c] <= 0) {
            perClumpPool[c] = members[c];
            return;
        }
        const std::vector<StrandGroup> groups = splitClump(s, members[c], skin, perClump[c], layers);
        int parity = 0;
        for (const StrandGroup& g : groups) {
            if (static_cast<int>(g.members.size()) < minStrands) {
                perClumpPool[c].insert(perClumpPool[c].end(), g.members.begin(), g.members.end());
                continue;
            }
            CardSpec spec;
            spec.clump = static_cast<int>(c);
            spec.layer = g.layer;
            spec.ruffled = groups.size() > 1;
            spec.parity = parity++;
            std::vector<CardMesh::Card> cards = buildGroupCards(s, g.members, skin, settings, spec, &perClumpStrays[c]);
            for (std::size_t j = 0; j < cards.size(); ++j) {
                spec.turn = static_cast<int>(j);
                spec.crossing = static_cast<int>(cards.size());
                perClumpSpecs[c].push_back(spec);
                perClumpCards[c].push_back(std::move(cards[j]));
            }
        }
        // The clump's strands in groups too small for a card join the card of the clump they
        // lie closest to along their length (and its cross), which is rebuilt to fit them.
        std::vector<CardMesh::Card>& cards = perClumpCards[c];
        if (cards.empty() || perClumpPool[c].empty()) return;
        std::vector<char> dirty(cards.size(), 0);
        for (const std::size_t si : perClumpPool[c]) {
            std::size_t best = 0;
            float bestD = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < cards.size(); ++i) {
                if (perClumpSpecs[c][i].turn > 0) continue;  // one of a cross: alike
                const float d = strandCardDistance(s, si, cards[i]);
                if (d < bestD) bestD = d, best = i;
            }
            for (std::size_t i = 0; i < cards.size(); ++i) {
                if (perClumpSpecs[c][i].parity != perClumpSpecs[c][best].parity) continue;
                cards[i].strands.push_back(si);
                dirty[i] = 1;
            }
        }
        perClumpPool[c].clear();
        for (std::size_t i = 0; i < cards.size(); ++i) {
            if (!dirty[i]) continue;
            std::sort(cards[i].strands.begin(), cards[i].strands.end());
            std::vector<CardMesh::Card> rebuilt = buildGroupCards(s, cards[i].strands, skin, settings, perClumpSpecs[c][i], &perClumpStrays[c]);
            if (!rebuilt.empty()) cards[i] = std::move(rebuilt.front());
        }
    }, 1);
    std::vector<char> isStray(s.strandCount(), 0);
    for (std::size_t c = 0; c < members.size(); ++c) {
        for (std::size_t j = 0; j < perClumpCards[c].size(); ++j) {
            out.push_back(std::move(perClumpCards[c][j]));
            specs.push_back(perClumpSpecs[c][j]);
            if (specs.back().turn == 0) ++groups;
            specs.back().group = groups - 1;
        }
        pool.insert(pool.end(), perClumpPool[c].begin(), perClumpPool[c].end());
        for (const std::size_t i : perClumpStrays[c]) isStray[i] = 1;
        pool.insert(pool.end(), perClumpStrays[c].begin(), perClumpStrays[c].end());
    }
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (isFlyaway[i]) isStray[i] = 1;  // (never joining a card either)
    }
    // Over budget with crossed cards: the smallest groups give up their extra cards first
    // (the braids' crosses last).
    if (hairMax > 0 && out.size() > static_cast<std::size_t>(hairMax)) {
        std::vector<std::size_t> extras;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (specs[i].turn > 0) extras.push_back(i);
        }
        std::stable_sort(extras.begin(), extras.end(), [&](std::size_t a, std::size_t b) {
            if ((specs[a].braid >= 0) != (specs[b].braid >= 0)) return specs[a].braid < 0;
            return out[a].strands.size() < out[b].strands.size();
        });
        std::vector<char> drop(out.size(), 0);
        std::size_t count = out.size();
        for (const std::size_t i : extras) {
            if (count <= static_cast<std::size_t>(hairMax)) break;
            drop[i] = 1;
            --count;
        }
        std::vector<CardMesh::Card> kept;
        std::vector<CardSpec> keptSpecs;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (drop[i]) continue;
            kept.push_back(std::move(out[i]));
            keptSpecs.push_back(specs[i]);
        }
        out = std::move(kept);
        specs = std::move(keptSpecs);
    }

    // Strands no card holds (and strays left out of theirs): the farthest from any card get
    // thin flyaway cards of their own (up to the budget), the rest join the nearest card
    // within reach; the others are dropped.
    std::sort(pool.begin(), pool.end());
    pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    if (!pool.empty() && !out.empty()) {
        std::vector<Vec3f> cardRoots(out.size());
        std::vector<float> rootWidths;
        for (std::size_t i = 0; i < out.size(); ++i) cardRoots[i] = out[i].refCenter.front(), rootWidths.push_back(out[i].refWidth.front());
        std::nth_element(rootWidths.begin(), rootWidths.begin() + static_cast<std::ptrdiff_t>(rootWidths.size() / 2), rootWidths.end());
        const float typical = std::max(rootWidths[rootWidths.size() / 2], 1e-6f);
        const PointGrid cardGrid(cardRoots, 2.0f * typical);
        // Each pool strand's nearest card (within reach) and its distance (none: far).
        struct Loose {
            std::size_t strand;
            long long card;
            float distance;
        };
        std::vector<Loose> loose(pool.size());
        parallelFor(0, pool.size(), [&](std::size_t p) {
            const Vec3f root = s.strandPoints(pool[p])[0];
            const auto [card, distance] = cardGrid.nearest(root, 3.0f * typical);
            loose[p] = {pool[p], card, card >= 0 ? distance : std::numeric_limits<float>::max()};
        });
        // In reach of a card's root: about its root width from its center. Strays left out
        // of a card never rejoin one (they would widen it again): flyaway cards, first.
        for (Loose& l : loose) {
            if (isStray[l.strand] || (l.card >= 0 && l.distance > std::max(out[static_cast<std::size_t>(l.card)].refWidth.front(), typical))) {
                l.card = -1, l.distance = std::numeric_limits<float>::max();
            }
        }
        // The Flyaway node's strays first (the longest, the most seen, first), then the farthest
        // from any card.
        std::vector<float> flyawayLength(pool.size(), 0.0f);
        for (std::size_t p = 0; p < pool.size(); ++p) {
            if (isFlyaway[pool[p]]) flyawayLength[p] = arcLength(s.strandPoints(pool[p]), s.strandPointCount(pool[p]));
        }
        std::vector<std::size_t> order(pool.size());
        for (std::size_t p = 0; p < pool.size(); ++p) order[p] = p;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (flyawayLength[a] != flyawayLength[b]) return flyawayLength[a] > flyawayLength[b];
            return loose[a].distance > loose[b].distance;
        });
        std::vector<char> taken(pool.size(), 0);
        std::vector<Vec3f> poolRoots(pool.size());
        for (std::size_t p = 0; p < pool.size(); ++p) poolRoots[p] = s.strandPoints(pool[p])[0];
        const PointGrid poolGrid(poolRoots, typical);
        std::vector<std::vector<std::size_t>> flyaways;
        int budget = settings.flyawayCards;
        if (hairMax > 0) budget = std::min(budget, hairMax - static_cast<int>(out.size()));
        for (const std::size_t p : order) {
            if (static_cast<int>(flyaways.size()) >= budget || loose[p].card >= 0) break;  // the rest can join a card
            if (taken[p]) continue;
            // It and the loose strands rooted beside it, on one thin card; a Flyaway node's stray
            // alone (one strand's texture).
            std::vector<std::size_t> group;
            if (isFlyaway[pool[p]]) {
                taken[p] = 1, group.push_back(pool[p]);
            } else {
                for (const std::size_t q : poolGrid.within(poolRoots[p], 0.5f * typical)) {
                    if (!taken[q] && !isFlyaway[pool[q]]) taken[q] = 1, group.push_back(pool[q]);
                }
            }
            std::sort(group.begin(), group.end());
            flyaways.push_back(std::move(group));
        }
        std::vector<std::vector<CardMesh::Card>> flyawayCards(flyaways.size());
        parallelFor(0, flyaways.size(), [&](std::size_t f) {
            CardSpec spec;
            spec.flyaway = true;
            flyawayCards[f] = buildGroupCards(s, flyaways[f], skin, settings, spec);
        }, 1);
        // The rest join their nearest card (every card of its cross), which is rebuilt to fit them.
        std::vector<std::vector<std::size_t>> siblings(static_cast<std::size_t>(groups));
        for (std::size_t i = 0; i < out.size(); ++i) siblings[static_cast<std::size_t>(specs[i].group)].push_back(i);
        std::vector<char> dirty(out.size(), 0);
        for (std::size_t p = 0; p < pool.size(); ++p) {
            if (taken[p] || loose[p].card < 0) continue;
            for (const std::size_t i : siblings[static_cast<std::size_t>(specs[static_cast<std::size_t>(loose[p].card)].group)]) {
                out[i].strands.push_back(pool[p]);
                dirty[i] = 1;
            }
        }
        parallelFor(0, out.size(), [&](std::size_t i) {
            if (!dirty[i]) return;
            std::sort(out[i].strands.begin(), out[i].strands.end());
            std::vector<CardMesh::Card> rebuilt = buildGroupCards(s, out[i].strands, skin, settings, specs[i]);
            if (!rebuilt.empty()) out[i] = std::move(rebuilt.front());
        }, 1);
        for (auto& cards : flyawayCards) {
            for (CardMesh::Card& card : cards) out.push_back(std::move(card));
        }
    }

    if (settings.segmentsByLength && !out.empty()) {
        // Segments by length: short cards need fewer edge loops (their UVs don't change).
        std::vector<float> lengths;
        for (const CardMesh::Card& c : out) lengths.push_back(arcLength(c.refCenter));
        std::vector<float> sorted = lengths;
        std::sort(sorted.begin(), sorted.end());
        const float reference = std::max(sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(0.95f * static_cast<float>(sorted.size())))], 1e-9f);
        const int most = std::max(settings.segments, 1);
        const int least = std::clamp(settings.minSegments, 1, most);
        for (std::size_t c = 0; c < out.size(); ++c) {
            if (out[c].braidSection >= 0) continue;  // (its loops follow its turns: buildTubeCard)
            const int segments = std::clamp(static_cast<int>(std::lround(static_cast<float>(most) * lengths[c] / reference)), least, most);
            out[c].stations = segments + 1;
            sampleStations(out[c]);
        }
    }
    if (settings.variations > 0) shareCardTextures(out, &s, settings.variations, settings.mirror);
    // The feathers' cards after the hair's (their variations shared among themselves already).
    const int offset = static_cast<int>(out.size());
    for (CardMesh::Card& card : featherCards) {
        if (card.variationOf >= 0) card.variationOf += offset;
        out.push_back(std::move(card));
    }
    // Cards through each other, or too close, pushed apart (the mesh alone).
    if (settings.separation > 0.0f || settings.featherSeparation > 0.0f) {
        separateCards(out, settings.separation, settings.featherSeparation, settings.separationIterations);
    }
    // Each card's own strands' colors, root to tip: its tint over the texture it shares.
    const detail::StrandColors colors = detail::colorsOf(s, CardBakeSettings{}.color);
    parallelFor(0, out.size(), [&](std::size_t c) {
        out[c].colors = detail::cardColors(out[c], s, colors);
        out[c].iridescence = detail::cardIridescence(out[c], s, colors);
    }, 16);
    return out;
}

std::vector<float> cardFitDistances(const StrandSet& s, const CardMesh& cards, int samples, bool allStrands) {
    const auto n = static_cast<std::size_t>(std::max(samples, 2));
    // Per strand, the distance of each of its samples to the nearest of the cards that hold it
    // (allStrands: to the nearest card of all, for every strand).
    std::vector<std::vector<std::size_t>> cardsOf(s.strandCount());
    for (std::size_t c = 0; c < cards.cards.size(); ++c) {
        for (const std::size_t si : cards.cards[c].strands) {
            if (si < cardsOf.size()) cardsOf[si].push_back(c);
        }
    }
    std::vector<std::vector<float>> per(s.strandCount());
    parallelFor(0, s.strandCount(), [&](std::size_t si) {
        if (s.strandPointCount(si) < 2) return;
        std::vector<std::size_t> against = cardsOf[si];
        if (allStrands) {
            against.resize(cards.cards.size());
            for (std::size_t c = 0; c < cards.cards.size(); ++c) against[c] = c;
        }
        if (against.empty()) return;
        const std::vector<Vec3f> pts = resampleByArcLength(s.strandPoints(si), s.strandPointCount(si), n);
        std::vector<float> best(n, std::numeric_limits<float>::max());
        for (const std::size_t c : against) {
            const CardMesh::Card& card = cards.cards[c];
            const float length = arcLength(CardShape(card).center);
            int segment = -1;
            for (std::size_t k = 0; k < n; ++k) best[k] = std::min(best[k], pointCardDistance(card, length, pts[k], segment));
        }
        per[si] = std::move(best);
    }, 64);
    std::vector<float> out;
    for (const auto& d : per) out.insert(out.end(), d.begin(), d.end());
    std::sort(out.begin(), out.end());
    return out;
}

CardMesh buildCards(const StrandSet& s, const ScalpMesh* scalp, const CardSettings& settings) {
    CardMesh mesh;
    mesh.cards = makeCards(s, scalp, settings);
    layoutCards(mesh, settings.padding);
    return mesh;
}

}  // namespace groomlab
