// SPDX-License-Identifier: Apache-2.0
// Lock pieces joined end to end, and repeated pieces found. See guide_merge.h.

#include "groomlab/guide_merge.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace groomlab {

namespace {

constexpr float kDegrees = 3.14159265358979f / 180.0f;

float polylineLength(const std::vector<Vec3f>& p) {
    float l = 0.0f;
    for (std::size_t i = 1; i < p.size(); ++i) l += (p[i] - p[i - 1]).length();
    return l;
}

// The inward tangent at one end (`back`: the last point): from the end into the chain, over its
// first `along` of length (the chain's far end when it is shorter).
Vec3f inwardTangent(const std::vector<Vec3f>& p, bool back, float along) {
    const std::size_t n = p.size();
    const Vec3f& e = back ? p[n - 1] : p[0];
    float walked = 0.0f;
    Vec3f to = back ? p[0] : p[n - 1];
    for (std::size_t k = 1; k < n; ++k) {
        const Vec3f& a = back ? p[n - k] : p[k - 1];
        const Vec3f& b = back ? p[n - 1 - k] : p[k];
        walked += (b - a).length();
        if (walked >= along) {
            to = b;
            break;
        }
    }
    const Vec3f d = to - e;
    const float l = d.length();
    return l > 1e-12f ? d * (1.0f / l) : Vec3f(0.0f, 0.0f, 0.0f);
}

// The distance from `q` to a polyline and the unit direction of the segment it is nearest.
float distanceToPolyline(const Vec3f& q, const std::vector<Vec3f>& p, Vec3f& direction) {
    float best = std::numeric_limits<float>::max();
    direction = Vec3f(0.0f, 0.0f, 0.0f);
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const Vec3f s = p[i + 1] - p[i];
        const float ll = s.dot(s);
        float t = ll > 1e-20f ? (q - p[i]).dot(s) / ll : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float d = (q - (p[i] + s * t)).length();
        if (d < best) {
            best = d;
            direction = ll > 1e-20f ? s * (1.0f / std::sqrt(ll)) : direction;
        }
    }
    if (p.size() == 1) best = (q - p[0]).length();
    return best;
}

}  // namespace

PieceMergeResult mergePieces(const std::vector<std::vector<Vec3f>>& pieces, const PieceMergeOptions& options) {
    PieceMergeResult out;
    out.chains = pieces;
    out.members.resize(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) out.members[i] = {static_cast<int>(i)};
    const int levels = std::max(1, options.levels);
    const float d0 = std::max(1e-9f, options.distanceStart), d1 = std::max(d0, options.distanceEnd);
    const float a0 = std::max(1e-3f, options.angleStart), a1 = std::max(a0, options.angleEnd);
    const float along = options.tangentLength > 0.0f ? options.tangentLength : 2.0f * d1;
    struct End {
        std::size_t chain;
        bool back;
        Vec3f p, t;
    };
    struct Pair {
        float distance;
        std::size_t a, b;  // into the ends
    };
    for (int level = 0; level < levels; ++level) {
        // (log-linear from start to end: HairGS's exponential schedule)
        const float f = levels > 1 ? static_cast<float>(level) / static_cast<float>(levels - 1) : 1.0f;
        const float d = d0 * std::pow(d1 / d0, f);
        const float angle = a0 * std::pow(a1 / a0, f);
        const float cosMin = std::cos(angle * kDegrees);
        for (int pass = 0; pass < std::max(1, options.passesPerLevel); ++pass) {
            std::vector<End> ends;
            for (std::size_t c = 0; c < out.chains.size(); ++c) {
                const std::vector<Vec3f>& p = out.chains[c];
                if (p.size() < 2) continue;
                ends.push_back({c, false, p.front(), inwardTangent(p, false, along)});
                ends.push_back({c, true, p.back(), inwardTangent(p, true, along)});
            }
            std::vector<Pair> pairs;
            for (std::size_t i = 0; i < ends.size(); ++i)
                for (std::size_t j = i + 1; j < ends.size(); ++j) {
                    const End& ea = ends[i];
                    const End& eb = ends[j];
                    if (ea.chain == eb.chain) continue;  // (its own far end: no loops)
                    const Vec3f gap = eb.p - ea.p;
                    const float distance = gap.length();
                    if (distance > d) continue;
                    // (chain a leaves through its end the way -t_a; chain b goes on from its end the way t_b)
                    if ((ea.t * -1.0f).dot(eb.t) < cosMin) continue;
                    Vec3f u = eb.t - ea.t;
                    const float ul = u.length();
                    if (ul < 1e-9f) continue;
                    u = u * (1.0f / ul);
                    const float lengthwise = gap.dot(u);
                    const float across = (gap - u * lengthwise).length();
                    if (across > options.lateralShare * d || lengthwise < -options.lateralShare * d) continue;
                    pairs.push_back({distance, i, j});
                }
            std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.distance < y.distance; });
            std::vector<char> chainUsed(out.chains.size(), 0);
            std::vector<std::pair<std::size_t, std::size_t>> accepted;
            for (const Pair& pr : pairs) {
                const End& ea = ends[pr.a];
                const End& eb = ends[pr.b];
                if (chainUsed[ea.chain] || chainUsed[eb.chain]) continue;
                chainUsed[ea.chain] = chainUsed[eb.chain] = 1;
                accepted.push_back({pr.a, pr.b});
            }
            std::vector<char> gone(out.chains.size(), 0);
            for (const auto& [ia, ib] : accepted) {
                const End& ea = ends[ia];
                const End& eb = ends[ib];
                // (chain a turned to end at a, chain b to start at b; the two ends become their midpoint)
                std::vector<Vec3f> a = out.chains[ea.chain], b = out.chains[eb.chain];
                std::vector<int> ma = out.members[ea.chain], mb = out.members[eb.chain];
                bool reversedA = false;
                if (!ea.back) {
                    std::reverse(a.begin(), a.end());
                    std::reverse(ma.begin(), ma.end());
                    reversedA = true;
                }
                if (eb.back) {
                    std::reverse(b.begin(), b.end());
                    std::reverse(mb.begin(), mb.end());
                }
                std::vector<Vec3f> joined(a.begin(), a.end() - 1);
                joined.push_back((ea.p + eb.p) * 0.5f);
                joined.insert(joined.end(), b.begin() + 1, b.end());
                ma.insert(ma.end(), mb.begin(), mb.end());
                // (kept in chain a's own direction)
                if (reversedA) {
                    std::reverse(joined.begin(), joined.end());
                    std::reverse(ma.begin(), ma.end());
                }
                out.chains[ea.chain] = std::move(joined);
                out.members[ea.chain] = std::move(ma);
                gone[eb.chain] = 1;
            }
            if (!accepted.empty()) {
                std::vector<std::vector<Vec3f>> chains;
                std::vector<std::vector<int>> members;
                for (std::size_t c = 0; c < out.chains.size(); ++c) {
                    if (gone[c]) continue;
                    chains.push_back(std::move(out.chains[c]));
                    members.push_back(std::move(out.members[c]));
                }
                out.chains = std::move(chains);
                out.members = std::move(members);
            }
            out.mergesPerPass.push_back(static_cast<int>(accepted.size()));
            out.passDistance.push_back(d);
            out.passAngle.push_back(angle);
            out.merges += static_cast<int>(accepted.size());
            if (accepted.empty()) break;
        }
    }
    return out;
}

std::vector<int> duplicatePieces(const std::vector<std::vector<Vec3f>>& pieces, const DuplicatePieceOptions& options) {
    const std::size_t n = pieces.size();
    std::vector<int> of(n, -1);
    std::vector<float> length(n);
    for (std::size_t i = 0; i < n; ++i) length[i] = polylineLength(pieces[i]);
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return length[a] > length[b]; });
    const float cosMin = std::cos(options.angle * kDegrees);
    std::vector<std::size_t> kept;
    for (std::size_t i : order) {
        const std::vector<Vec3f>& p = pieces[i];
        if (p.size() < 2) {
            kept.push_back(i);
            continue;
        }
        for (std::size_t k : kept) {
            const std::vector<Vec3f>& q = pieces[k];
            if (q.size() < 2) continue;
            int on = 0;
            for (std::size_t m = 0; m < p.size(); ++m) {
                const Vec3f t = p[std::min(m + 1, p.size() - 1)] - p[m > 0 ? m - 1 : 0];
                const float tl = t.length();
                Vec3f dir;
                if (distanceToPolyline(p[m], q, dir) > options.distance) continue;
                if (tl > 1e-12f && std::abs(t.dot(dir)) < cosMin * tl) continue;
                ++on;
            }
            if (static_cast<float>(on) >= options.share * static_cast<float>(p.size())) {
                of[i] = static_cast<int>(k);
                break;
            }
        }
        if (of[i] < 0) kept.push_back(i);
    }
    return of;
}

}  // namespace groomlab
