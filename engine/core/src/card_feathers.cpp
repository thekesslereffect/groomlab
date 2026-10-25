// SPDX-License-Identifier: Apache-2.0
// Feathers' cards: one card per feather (a Propagate node's shaft and barbs), see
// CardSettings::featherMode.

#include "groomlab/cards.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

#include "cards_internal.h"

namespace groomlab {

// What the card files share (cards_internal.h).
using detail::kRefStations;
using detail::kFeatherSideChannel;
using detail::kShaftUChannel;
using detail::sampleStations;
using detail::seatCard;
using detail::cardFrames;
using detail::Feather;

namespace {

// A feather measured along its shaft at kRefStations: the shaft carried on past its ends as far
// as the barbs reach, the vane's frame (side: toward the right vane, side +1, in the plane the
// barbs spread in; normal = side x tangent, out of the vane's top), and how far the barbs and
// the rachis reach toward +side (right) and -side (left) at each station. A down feather's
// barbs spiral all around the shaft: its frame faces out of the scalp and both reaches are the
// barbs' distance from the shaft.
struct FeatherShape {
    std::vector<Vec3f> center, tangent, side, normal;
    std::vector<float> right, left;
    // How each vane leaves the vane plane at each station: the tangent of its angle out of it
    // (toward the normal positive), right vane and left (0 for a down feather).
    std::vector<float> foldRight, foldLeft;
    float length = 0.0f;
    float strandWidth = 0.0f;
    Vec3f root;  // where the shaft grows
    float rootAngle = 0.0f, tipAngle = 0.0f;  // the barbs' mean angle from the shaft at their root and tip (radians)
    bool down = false;
    bool valid = false;
};

FeatherShape measureFeather(const StrandSet& s, const Feather& feather, const ScalpMesh* scalp) {
    FeatherShape f;
    const std::vector<float>* sides = s.strandChannel(kFeatherSideChannel);
    const std::vector<float>* shaftUs = s.strandChannel(kShaftUChannel);
    std::vector<std::size_t> barbs;
    int around = 0, vaned = 0;
    for (const std::size_t i : feather.strands) {
        if (static_cast<long long>(i) == feather.shaft) continue;
        if (s.strandPointCount(i) < 2 || !(arcLength(s.strandPoints(i), s.strandPointCount(i)) > 0.0f)) continue;
        barbs.push_back(i);
        (sides && std::abs((*sides)[i]) > 0.5f ? vaned : around) += 1;
    }
    f.down = around > vaned;

    // The shaft: the kept one, else a line through the barbs' roots in order along it (both
    // vanes' barbs grow at the same places: their roots averaged).
    std::vector<Vec3f> shaft;
    if (feather.shaft >= 0) {
        const auto i = static_cast<std::size_t>(feather.shaft);
        shaft.assign(s.strandPoints(i), s.strandPoints(i) + s.strandPointCount(i));
    } else if (shaftUs) {
        std::vector<std::pair<float, std::size_t>> order;
        for (const std::size_t b : barbs) order.emplace_back((*shaftUs)[b], b);
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        float at = -1.0f, count = 0.0f;
        Vec3f sum{};
        for (const auto& [u, b] : order) {
            if (count > 0.0f && std::abs(u - at) > 1e-5f) shaft.push_back(sum / count), sum = Vec3f{}, count = 0.0f;
            at = u;
            sum += s.strandPoints(b)[0];
            count += 1.0f;
        }
        if (count > 0.0f) shaft.push_back(sum / count);
    }
    if (shaft.size() < 2 || !(arcLength(shaft.data(), shaft.size()) > 0.0f)) return f;
    const float shaftLength = arcLength(shaft.data(), shaft.size());
    f.root = shaft.front();
    const std::vector<Vec3f> even = resampleByArcLength(shaft.data(), shaft.size(), 16);

    // Samples along every barb, then along the rachis.
    constexpr std::size_t kBarbSamples = 8;
    std::vector<Vec3f> samples;
    samples.reserve(barbs.size() * kBarbSamples + even.size());
    std::vector<Vec3f> pts(kBarbSamples);
    for (const std::size_t b : barbs) {
        resampleByArcLengthInto(s.strandPoints(b), s.strandPointCount(b), pts.data(), kBarbSamples);
        samples.insert(samples.end(), pts.begin(), pts.end());
    }
    samples.insert(samples.end(), even.begin(), even.end());

    // Carried on past the tip (and back past the root) as far as any barb reaches.
    const Vec3f tipDir = (even.back() - even[even.size() - 2]).normalized();
    const Vec3f rootDir = (even[1] - even[0]).normalized();
    float ahead = 0.0f, behind = 0.0f;
    for (const Vec3f& q : samples) {
        ahead = std::max(ahead, (q - even.back()).dot(tipDir));
        behind = std::max(behind, (even.front() - q).dot(rootDir));
    }
    std::vector<Vec3f> longer;
    if (behind > 0.005f * shaftLength) longer.push_back(even.front() - rootDir * (behind + 0.01f * shaftLength));
    longer.insert(longer.end(), even.begin(), even.end());
    if (ahead > 0.005f * shaftLength) longer.push_back(even.back() + tipDir * (ahead + 0.01f * shaftLength));
    f.center = resampleByArcLength(longer.data(), longer.size(), kRefStations);
    f.length = arcLength(f.center);
    if (!(f.length > 0.0f)) return f;
    constexpr std::size_t K = kRefStations;
    f.tangent.resize(K);
    for (std::size_t k = 0; k < K; ++k) f.tangent[k] = (f.center[std::min(k + 1, K - 1)] - f.center[k > 0 ? k - 1 : 0]).normalized();
    auto stationOf = [&](const Vec3f& p) {
        std::size_t best = 0;
        float bestD = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < K; ++k) {
            const float d = (f.center[k] - p).dot(f.center[k] - p);
            if (d < bestD) bestD = d, best = k;
        }
        return best;
    };

    // The vane's side: the way each barb heads across the shaft (toward its own side), summed
    // near where it grows; stations no barb grows at (the bare quill, the carried-on tip) take
    // the nearest that has one. A down feather (or one with no sides) faces out of the scalp.
    std::vector<Vec3f> across(K, Vec3f{});
    bool any = false;
    if (!f.down && sides) {
        for (const std::size_t b : barbs) {
            const float side = (*sides)[b];
            if (std::abs(side) < 0.5f) continue;
            const Vec3f* bp = s.strandPoints(b);
            const std::size_t n = s.strandPointCount(b);
            const std::size_t k = stationOf(bp[0]);
            Vec3f d = bp[n - 1] - bp[0];
            d = (d - f.tangent[k] * d.dot(f.tangent[k])) * (side > 0.0f ? 1.0f : -1.0f);
            for (std::size_t j = k > 2 ? k - 2 : 0; j <= std::min(k + 2, K - 1); ++j) across[j] += d;
            any = any || d.length() > 0.0f;
        }
    }
    if (any) {
        // A station says which way the vane lies only where its barbs head across together: where
        // they nearly cancel (curled back on themselves near the tip: a sum a sliver of the
        // others') or head against the stations nearer the strongest one, the side would flip
        // there and shear the card. Those take their neighbours'.
        std::vector<float> strength(K, 0.0f), nonzero;
        for (std::size_t k = 0; k < K; ++k) {
            strength[k] = across[k].length();
            if (strength[k] > 1e-12f) nonzero.push_back(strength[k]);
        }
        std::nth_element(nonzero.begin(), nonzero.begin() + static_cast<std::ptrdiff_t>(nonzero.size() / 2), nonzero.end());
        const float weak = 0.15f * nonzero[nonzero.size() / 2];
        std::vector<char> known(K, 0);
        for (std::size_t k = 0; k < K; ++k) known[k] = strength[k] > std::max(weak, 1e-12f);
        const auto strongest = static_cast<std::size_t>(std::max_element(strength.begin(), strength.end()) - strength.begin());
        for (const int step : {1, -1}) {
            std::size_t last = strongest;
            for (auto k = static_cast<std::ptrdiff_t>(strongest) + step; k >= 0 && k < static_cast<std::ptrdiff_t>(K); k += step) {
                const auto at = static_cast<std::size_t>(k);
                if (!known[at]) continue;
                if (across[at].dot(across[last]) < 0.0f) {
                    known[at] = 0;
                    continue;
                }
                last = at;
            }
        }
        for (std::size_t k = 0; k < K; ++k) {
            if (!known[k]) across[k] = Vec3f{};
        }
        for (std::size_t k = 1; k < K; ++k) {
            if (!known[k] && known[k - 1]) across[k] = across[k - 1], known[k] = 1;
        }
        for (std::size_t k = K - 1; k-- > 0;) {
            if (!known[k] && known[k + 1]) across[k] = across[k + 1], known[k] = 1;
        }
        for (Vec3f& a : across) a = a.normalized();
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<Vec3f> smooth = across;
            for (std::size_t k = 1; k + 1 < K; ++k) smooth[k] = across[k - 1] * 0.25f + across[k] * 0.5f + across[k + 1] * 0.25f;
            across = smooth;
        }
        f.side.resize(K);
        f.normal.resize(K);
        for (std::size_t k = 0; k < K; ++k) {
            Vec3f side = across[k] - f.tangent[k] * across[k].dot(f.tangent[k]);
            side = side.length() > 1e-6f ? side.normalized() : anyPerpendicular(f.tangent[k]);
            f.side[k] = side;
            f.normal[k] = side.cross(f.tangent[k]).normalized();
        }
    } else {
        const std::vector<Frame> frames = cardFrames(f.center, scalp);
        for (const Frame& fr : frames) f.side.push_back(fr.binormal), f.normal.push_back(fr.normal);
    }

    // Reach each way across, at each station (and its neighbours): a down feather's is the
    // barbs' distance from the shaft, whichever way its crossed cards turn.
    f.right.assign(K, 0.0f);
    f.left.assign(K, 0.0f);
    for (const Vec3f& q : samples) {
        const std::size_t k = stationOf(q);
        const Vec3f d = q - f.center[k];
        float r = d.dot(f.side[k]), l = -r;
        if (f.down) r = l = (d - f.tangent[k] * d.dot(f.tangent[k])).length();
        for (std::size_t j = k > 0 ? k - 1 : 0; j <= std::min(k + 1, K - 1); ++j) {
            f.right[j] = std::max(f.right[j], r);
            f.left[j] = std::max(f.left[j], l);
        }
    }
    // No notches between wider stations (a dip narrower than a fifth of the card fills to the
    // lower of its flanks), as a hair card's width.
    for (std::vector<float>* reach : {&f.right, &f.left}) {
        const std::vector<float> w = *reach;
        constexpr std::size_t kReach = 3;
        for (std::size_t k = 0; k < K; ++k) {
            float before = 0.0f, after = 0.0f;
            for (std::size_t j = k > kReach ? k - kReach : 0; j <= k; ++j) before = std::max(before, w[j]);
            for (std::size_t j = k; j <= std::min(k + kReach, K - 1); ++j) after = std::max(after, w[j]);
            (*reach)[k] = std::max(w[k], std::min(before, after));
        }
    }

    // How each vane leaves the vane plane (lift, droop, a cupped vane, one vane up and the other
    // down): at each station, the direction its barbs' samples there take across the shaft (x out
    // along the vane's own side, y along the normal), weighted by their squared distance from the
    // shaft so the outer parts, where the card's edge is, count the most. Its angle is smoothed
    // along the shaft and kept within 60 degrees (a vane folding past that is a tuft, not a
    // card's half); a station where a vane has no barbs folds flat.
    f.foldRight.assign(K, 0.0f);
    f.foldLeft.assign(K, 0.0f);
    if (!f.down && sides) {
        std::vector<float> sx[2] = {std::vector<float>(K, 0.0f), std::vector<float>(K, 0.0f)};
        std::vector<float> sy[2] = {std::vector<float>(K, 0.0f), std::vector<float>(K, 0.0f)};
        for (const std::size_t b : barbs) {
            const float side = (*sides)[b];
            if (std::abs(side) < 0.5f) continue;
            const int h = side > 0.0f ? 0 : 1;  // 0 right, 1 left
            resampleByArcLengthInto(s.strandPoints(b), s.strandPointCount(b), pts.data(), kBarbSamples);
            for (std::size_t j = 1; j < kBarbSamples; ++j) {
                const std::size_t k = stationOf(pts[j]);
                Vec3f d = pts[j] - f.center[k];
                d = d - f.tangent[k] * d.dot(f.tangent[k]);
                const float x = d.dot(f.side[k]) * (h == 0 ? 1.0f : -1.0f), y = d.dot(f.normal[k]);
                const float wgt = x * x + y * y;
                sx[h][k] += wgt * x;
                sy[h][k] += wgt * y;
            }
        }
        constexpr float kMaxFold = 1.0471976f;  // 60 degrees
        for (int h = 0; h < 2; ++h) {
            std::vector<float> angle(K, 0.0f);
            for (std::size_t k = 0; k < K; ++k) {
                if (sx[h][k] * sx[h][k] + sy[h][k] * sy[h][k] > 0.0f) angle[k] = std::clamp(pm::atan2(sy[h][k], sx[h][k]), -kMaxFold, kMaxFold);
            }
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<float> smooth = angle;
                for (std::size_t k = 0; k < K; ++k) {
                    smooth[k] = 0.25f * angle[k > 0 ? k - 1 : 0] + 0.5f * angle[k] + 0.25f * angle[std::min(k + 1, K - 1)];
                }
                angle = smooth;
            }
            std::vector<float>& fold = h == 0 ? f.foldRight : f.foldLeft;
            for (std::size_t k = 0; k < K; ++k) fold[k] = pm::tan(angle[k]);
        }
    }

    // The barbs' mean angle from the shaft where they grow and at their tips (their curl).
    float angles = 0.0f;
    for (const std::size_t b : barbs) {
        const Vec3f* bp = s.strandPoints(b);
        const std::size_t n = s.strandPointCount(b);
        const Vec3f& t = f.tangent[stationOf(bp[0])];
        const Vec3f d0 = (bp[1] - bp[0]).normalized(), d1 = (bp[n - 1] - bp[n - 2]).normalized();
        f.rootAngle += pm::acos(std::clamp(d0.dot(t), -1.0f, 1.0f));
        f.tipAngle += pm::acos(std::clamp(d1.dot(t), -1.0f, 1.0f));
        angles += 1.0f;
    }
    if (angles > 0.0f) f.rootAngle /= angles, f.tipAngle /= angles;

    // The strands' width, as a hair card's (the default 1: not real widths).
    if (!s.widths().empty()) {
        float sum = 0.0f, n = 0.0f;
        for (const std::size_t i : feather.strands) {
            for (std::size_t p = 0; p < s.strandPointCount(i); ++p) sum += s.widths()[s.strandBegin(i) + p], n += 1.0f;
        }
        f.strandWidth = n > 0.0f ? sum / n : 0.0f;
        if (f.strandWidth == 1.0f) f.strandWidth = 0.0f;
    }
    if (f.strandWidth <= 0.0f) f.strandWidth = 0.002f * f.length;
    f.valid = true;
    return f;
}

// Where a feather's card lies across its frame: its middle `offset` off the shaft toward +side
// (asymmetric vanes: the shaft sits off the texture's middle, at one place all along), its width
// per station and its texture's width.
struct FeatherLayout {
    float offset = 0.0f;
    std::vector<float> width;
    float textureWidth = 0.0f;
    FeatherLayout scaled(float by, bool mirrored) const {
        FeatherLayout out = *this;
        out.offset = offset * by * (mirrored ? -1.0f : 1.0f);
        for (float& w : out.width) w *= by;
        out.textureWidth = textureWidth * by;
        return out;
    }
};

FeatherLayout layoutFeather(const FeatherShape& f, const CardSettings& settings) {
    FeatherLayout out;
    const float right = *std::max_element(f.right.begin(), f.right.end());
    const float left = *std::max_element(f.left.begin(), f.left.end());
    out.offset = f.down ? 0.0f : 0.5f * (right - left);
    const float margin = std::clamp(settings.margin, 0.0f, 0.45f);
    for (std::size_t k = 0; k < f.right.size(); ++k) {
        const float half = std::max(f.right[k] - out.offset, f.left[k] + out.offset) + f.strandWidth;
        out.width.push_back(std::max(2.0f * half / (1.0f - 2.0f * margin), settings.minWidth));
    }
    out.textureWidth = *std::max_element(out.width.begin(), out.width.end());
    return out;
}

// A feather's card: along its shaft, turned `turn` radians round it (a down feather's cross),
// laid out as `layout` says; with `spine`, a column on the shaft and each half folded to its
// vane (Card::spine).
CardMesh::Card makeFeatherCard(const FeatherShape& f, const std::vector<std::size_t>& strands, const FeatherLayout& layout,
                               float turn, bool spine, const CardSettings& settings, const ScalpMesh* scalp) {
    CardMesh::Card card;
    card.strands = strands;
    card.stations = std::clamp(settings.featherSegments, 1, 256) + 1;
    card.across = 1;
    if (spine) {
        card.spine = true;
        card.across = 2;
        card.spineOffset = -layout.offset;  // the card's middle sits `offset` off the shaft toward +side
        card.refFoldRight = f.foldRight;
        card.refFoldLeft = f.foldLeft;
    }
    const float c = pm::cos(turn), sn = pm::sin(turn);
    for (std::size_t k = 0; k < kRefStations; ++k) {
        const Vec3f side = f.side[k] * c - f.normal[k] * sn;
        card.refSide.push_back(side);
        card.refNormal.push_back(f.normal[k] * c + f.side[k] * sn);
        card.refCenter.push_back(f.center[k] + side * layout.offset);
    }
    card.refWidth = layout.width;
    card.textureWidth = layout.textureWidth;
    // Clear of the scalp, as a hair card: a feather lying on the body sits a hair above it.
    if (scalp) {
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
    sampleStations(card);
    // Its base on the skin where the shaft grows, as a hair card's (the card reaches back past
    // the root as far as the barbs do, into the skin, and the clearance lifts it out again).
    if (scalp) seatCard(card, *scalp, f.root, f.strandWidth);
    return card;
}

// What a feather's texture looks like, for grouping alike feathers: its length, its vanes'
// outline (right then left, or swapped when `flip`), their asymmetry, the barbs' angle and
// curl, and its color.
std::vector<float> describeFeather(const FeatherShape& f, bool flip, const std::array<float, 3>& color) {
    std::vector<float> out{2.0f * pm::log(std::max(f.length, 1e-6f))};
    constexpr int kProfile = 8;
    float right = 0.0f, left = 0.0f;
    for (std::size_t k = 0; k < f.right.size(); ++k) right += f.right[k], left += f.left[k];
    for (int j = 0; j < kProfile; ++j) {
        const auto k = static_cast<std::size_t>(std::lround(static_cast<float>(j) * static_cast<float>(kRefStations - 1) / (kProfile - 1)));
        const float r = f.right[k] / f.length, l = f.left[k] / f.length;
        out.push_back(8.0f * (flip ? l : r));
        if (!f.down) out.push_back(8.0f * (flip ? r : l));
    }
    const float asymmetry = right + left > 0.0f ? (right - left) / (right + left) : 0.0f;
    out.push_back(2.0f * (flip ? -asymmetry : asymmetry));
    out.push_back(1.5f * f.rootAngle);
    out.push_back(1.5f * (f.tipAngle - f.rootAngle));
    for (const float c : color) out.push_back(4.0f * c);
    return out;
}

float featherAsymmetry(const FeatherShape& f) {
    float right = 0.0f, left = 0.0f;
    for (std::size_t k = 0; k < f.right.size(); ++k) right += f.right[k], left += f.left[k];
    return right + left > 0.0f ? (right - left) / (right + left) : 0.0f;
}

}  // namespace

namespace detail {

std::vector<Feather> groupFeathers(const StrandSet& s, std::vector<char>& member) {
    std::vector<Feather> out;
    const std::vector<float>* ids = s.strandChannel(kFeatherIdChannel);
    if (!ids) return out;
    const std::vector<float>* shafts = s.strandChannel(kIsShaftChannel);
    std::map<std::tuple<long long, std::int32_t, float, float>, std::size_t> index;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const long long id = std::llround((*ids)[i]);
        if (id < 0) continue;
        member[i] = 1;
        const StrandRoot& r = s.roots()[i];
        const auto key = std::make_tuple(id, r.face, r.face >= 0 ? r.u : 0.0f, r.face >= 0 ? r.v : 0.0f);
        const auto it = index.emplace(key, out.size()).first;
        if (it->second == out.size()) out.emplace_back();
        Feather& f = out[it->second];
        f.strands.push_back(i);
        if (shafts && (*shafts)[i] > 0.5f && f.shaft < 0 && s.strandPointCount(i) >= 2 &&
            arcLength(s.strandPoints(i), s.strandPointCount(i)) > 0.0f) {
            f.shaft = static_cast<long long>(i);
        }
    }
    return out;
}

std::vector<CardMesh::Card> makeFeatherCards(const StrandSet& s, const ScalpMesh* scalp, const CardSettings& settings,
                                             std::vector<char>& member) {
    std::vector<CardMesh::Card> out;
    const std::vector<Feather> feathers = groupFeathers(s, member);
    if (feathers.empty()) return out;
    std::vector<FeatherShape> shapes(feathers.size());
    parallelFor(0, feathers.size(), [&](std::size_t i) { shapes[i] = measureFeather(s, feathers[i], scalp); }, 1);
    const int cross = std::clamp(settings.crossCount, 2, 6);
    auto cardsOf = [&](std::size_t i) { return static_cast<std::size_t>(shapes[i].down ? cross : 1); };

    // The feathers that get cards: measured, long enough, and within the budget (the shortest
    // give way first, at least one kept).
    float longest = 0.0f;
    for (const FeatherShape& f : shapes) {
        if (f.valid) longest = std::max(longest, f.length);
    }
    const float minLength = std::clamp(settings.featherMinLength, 0.0f, 1.0f) * longest;
    std::vector<std::size_t> kept;
    for (std::size_t i = 0; i < feathers.size(); ++i) {
        if (shapes[i].valid && shapes[i].length >= minLength) kept.push_back(i);
    }
    if (settings.maxCards > 0) {
        std::size_t cards = 0;
        for (const std::size_t i : kept) cards += cardsOf(i);
        if (cards > static_cast<std::size_t>(settings.maxCards)) {
            std::vector<std::size_t> byLength = kept;
            std::stable_sort(byLength.begin(), byLength.end(), [&](std::size_t a, std::size_t b) { return shapes[a].length < shapes[b].length; });
            std::vector<char> drop(feathers.size(), 0);
            std::size_t left = kept.size();
            for (const std::size_t i : byLength) {
                if (cards <= static_cast<std::size_t>(settings.maxCards) || left <= 1) break;
                drop[i] = 1;
                cards -= cardsOf(i);
                --left;
            }
            kept.erase(std::remove_if(kept.begin(), kept.end(), [&](std::size_t i) { return drop[i] != 0; }), kept.end());
        }
    }
    if (kept.empty()) return out;

    std::vector<FeatherLayout> layouts(feathers.size());
    for (const std::size_t i : kept) layouts[i] = layoutFeather(shapes[i], settings);

    // Shared textures: alike feathers (vaned and down apart, `variations` between them by
    // their share) show the most typical one's, scaled to their length. A vaned feather is
    // described with its longer vane toward +side (flipped), so one whose longer vane is on
    // the other side shares it mirrored.
    std::vector<long long> repOf(feathers.size(), -1);
    std::vector<char> flipped(feathers.size(), 0);
    if (settings.variations > 0) {
        const StrandColors colors = colorsOf(s, {0.0f, 0.0f, 0.0f});
        std::size_t downs = 0;
        for (const std::size_t i : kept) downs += shapes[i].down ? 1 : 0;
        for (const bool down : {false, true}) {
            std::vector<std::size_t> group;
            for (const std::size_t i : kept) {
                if (shapes[i].down == down) group.push_back(i);
            }
            if (group.empty()) continue;
            std::size_t k = static_cast<std::size_t>(settings.variations);
            if (downs > 0 && downs < kept.size()) {
                k = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(static_cast<double>(settings.variations) *
                                                                                  static_cast<double>(group.size()) /
                                                                                  static_cast<double>(kept.size()))));
            }
            if (group.size() <= k) continue;  // each its own
            std::vector<std::vector<float>> features(group.size());
            for (std::size_t g = 0; g < group.size(); ++g) {
                const std::size_t i = group[g];
                flipped[i] = !down && settings.mirror && featherAsymmetry(shapes[i]) < 0.0f;
                std::array<float, 3> color{0.0f, 0.0f, 0.0f};
                for (const std::size_t si : feathers[i].strands) {
                    const auto c = colors(si);
                    for (int j = 0; j < 3; ++j) color[static_cast<std::size_t>(j)] += c[static_cast<std::size_t>(j)] / static_cast<float>(feathers[i].strands.size());
                }
                features[g] = describeFeather(shapes[i], flipped[i] != 0, color);
            }
            const std::vector<int> labels = kMeans(features, k, 30);
            const std::size_t groups = static_cast<std::size_t>(*std::max_element(labels.begin(), labels.end())) + 1;
            const std::size_t dims = features[0].size();
            std::vector<std::vector<float>> centers(groups, std::vector<float>(dims, 0.0f));
            std::vector<float> counts(groups, 0.0f);
            for (std::size_t g = 0; g < group.size(); ++g) {
                const auto l = static_cast<std::size_t>(labels[g]);
                for (std::size_t d = 0; d < dims; ++d) centers[l][d] += features[g][d];
                counts[l] += 1.0f;
            }
            for (std::size_t l = 0; l < groups; ++l) {
                for (float& x : centers[l]) x /= counts[l];
            }
            std::vector<long long> representative(groups, -1);
            for (std::size_t g = 0; g < group.size(); ++g) {
                const auto l = static_cast<std::size_t>(labels[g]);
                long long& r = representative[l];
                if (r < 0 || distance2(features[g], centers[l]) < distance2(features[static_cast<std::size_t>(r)], centers[l])) {
                    r = static_cast<long long>(g);
                }
            }
            for (std::size_t g = 0; g < group.size(); ++g) {
                const auto rep = static_cast<std::size_t>(representative[static_cast<std::size_t>(labels[g])]);
                if (rep != g) repOf[group[g]] = static_cast<long long>(group[rep]);
            }
        }
    }

    // The cards, feather by feather: each owner's (its first) card index first, for the others
    // to point at.
    std::vector<long long> firstCard(feathers.size(), -1);
    {
        long long next = 0;
        for (const std::size_t i : kept) firstCard[i] = next, next += static_cast<long long>(cardsOf(i));
    }
    std::vector<int> seen(feathers.size(), 0);  // shared feathers so far, per owner (alternate mirroring)
    int ordinal = 0;
    for (const std::size_t i : kept) {
        const FeatherShape& f = shapes[i];
        const long long rep = repOf[i] >= 0 ? repOf[i] : static_cast<long long>(i);
        const auto r = static_cast<std::size_t>(rep);
        const bool shared = rep != static_cast<long long>(i);
        const float scale = shared ? f.length / std::max(shapes[r].length, 1e-9f) : 1.0f;
        std::vector<std::size_t> strands = feathers[i].strands;
        std::sort(strands.begin(), strands.end());
        if (!f.down) {
            // Mirrored when its longer vane is on the other side from the owner's; alike
            // (near-symmetric) feathers alternate, so the repeat doesn't show.
            bool mirrored = shared && flipped[i] != flipped[r];
            if (shared && settings.mirror && std::abs(featherAsymmetry(f)) < 0.1f && std::abs(featherAsymmetry(shapes[r])) < 0.1f) {
                mirrored = ++seen[r] % 2 == 1;
            }
            CardMesh::Card card = makeFeatherCard(f, strands, shared ? layouts[r].scaled(scale, mirrored) : layouts[i], 0.0f,
                                                  settings.featherSpine, settings, scalp);
            card.variationOf = shared ? static_cast<int>(firstCard[r]) : -1;
            card.mirrored = mirrored;
            card.feather = ordinal;
            out.push_back(std::move(card));
        } else {
            // A cross: every card shows the owner's first card's texture (a down puff looks
            // alike from every side), alternate ones mirrored.
            const FeatherLayout layout = shared ? layouts[r].scaled(scale, false) : layouts[i];
            for (int j = 0; j < cross; ++j) {
                // Flat: the puff has no vanes to fold to, and the shaft already runs down its middle.
                CardMesh::Card card = makeFeatherCard(f, strands, layout, 3.14159265f * static_cast<float>(j) / static_cast<float>(cross),
                                                      false, settings, scalp);
                const bool owner = !shared && j == 0;
                if (settings.variations > 0 && !owner) {
                    card.variationOf = static_cast<int>(firstCard[r]);
                    card.mirrored = settings.mirror && j % 2 == 1;
                }
                card.feather = ordinal;
                out.push_back(std::move(card));
            }
        }
        ++ordinal;
    }
    return out;
}

}  // namespace detail

}  // namespace groomlab
