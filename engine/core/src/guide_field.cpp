// SPDX-License-Identifier: Apache-2.0
// The orientation field between guides and guides grown through it (guide_field.h).

#include "groomlab/guide_field.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/random.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
// The first solve holds the pore direction this loosely at the scalp's cells (a neighbour pair weighs
// 1): enough to give a field where no guide reaches, too little to pull the field between guides.
constexpr float kPoreWeight = 0.02f;

// Per-component sums (the three components are three systems sharing one matrix).
struct Sum3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

Sum3 dot3(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    Sum3 s;
    for (std::size_t i = 0; i < a.size(); ++i) {
        s.x += static_cast<double>(a[i].x) * b[i].x;
        s.y += static_cast<double>(a[i].y) * b[i].y;
        s.z += static_cast<double>(a[i].z) * b[i].z;
    }
    return s;
}

Vec3f mul(const Vec3f& a, const Vec3f& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// The sparse system (L + W + eps) t = W c on the volume's cells: L the graph Laplacian over the six
// neighbours (pairs across a part left out), W the constraints' weights.
struct System {
    std::vector<std::array<int, 6>> nb;  // neighbours (-1: none)
    std::vector<float> diag;

    std::vector<Vec3f> apply(const std::vector<Vec3f>& x) const {
        std::vector<Vec3f> y(x.size());
        parallelFor(0, x.size(), [&](std::size_t i) {
            Vec3f v = x[i] * diag[i];
            for (int j : nb[i])
                if (j >= 0) v -= x[static_cast<std::size_t>(j)];
            y[i] = v;
        }, 2048);
        return y;
    }
};

// Jacobi-preconditioned conjugate gradients, the three components at once. Stops when the residual
// scaled by the diagonal (how far a Jacobi step would move a cell, field units), its root mean square
// over the cells, is below `tolerance` in every component: a residual relative to the right-hand
// side would stop at once, the constraints' lambda^2 swamping the unconstrained cells. Returns the
// iterations and that residual.
std::pair<int, float> solve(const System& A, const std::vector<Vec3f>& b, std::vector<Vec3f>& x, int maxIterations, float tolerance) {
    const std::size_t n = b.size();
    std::vector<Vec3f> r = A.apply(x);
    for (std::size_t i = 0; i < n; ++i) r[i] = b[i] - r[i];
    std::vector<Vec3f> z(n), p(n);
    for (std::size_t i = 0; i < n; ++i) z[i] = r[i] / A.diag[i];
    p = z;
    Sum3 rz = dot3(r, z);
    auto scaled = [&]() {
        const Sum3 zz = dot3(z, z);
        return static_cast<float>(std::sqrt(std::max({zz.x, zz.y, zz.z}) / static_cast<double>(std::max<std::size_t>(1, n))));
    };
    float res = scaled();
    int it = 0;
    for (; it < maxIterations && res > tolerance; ++it) {
        const std::vector<Vec3f> Ap = A.apply(p);
        const Sum3 pAp = dot3(p, Ap);
        const Vec3f alpha(pAp.x > 0.0 ? static_cast<float>(rz.x / pAp.x) : 0.0f, pAp.y > 0.0 ? static_cast<float>(rz.y / pAp.y) : 0.0f,
                          pAp.z > 0.0 ? static_cast<float>(rz.z / pAp.z) : 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += mul(alpha, p[i]);
            r[i] -= mul(alpha, Ap[i]);
            z[i] = r[i] / A.diag[i];
        }
        const Sum3 rz2 = dot3(r, z);
        const Vec3f beta(rz.x > 0.0 ? static_cast<float>(rz2.x / rz.x) : 0.0f, rz.y > 0.0 ? static_cast<float>(rz2.y / rz.y) : 0.0f,
                         rz.z > 0.0 ? static_cast<float>(rz2.z / rz.z) : 0.0f);
        for (std::size_t i = 0; i < n; ++i) p[i] = z[i] + mul(beta, p[i]);
        rz = rz2;
        res = scaled();
    }
    return {it, res};
}

// Points along a polyline every `step` (its first point included), each with the direction there.
void walk(const std::vector<Vec3f>& pts, float step, const std::function<void(const Vec3f&, const Vec3f&, float)>& at) {
    float carry = 0.0f, s = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Vec3f d = pts[i] - pts[i - 1];
        const float len = d.length();
        if (len <= 0.0f) continue;
        const Vec3f t = d / len;
        float u = i == 1 ? 0.0f : step - carry;
        for (; u < len; u += step) at(pts[i - 1] + t * u, t, s + u);
        carry = len - (u - step);
        s += len;
    }
}

// How high a guide rides in the shell: the median over its points past its first 0.3 head radii of
// their height off the head, a share of the shell's thickness there (0 on the head, 1 its outside).
float rideShare(const HairShell& shell, const std::vector<Vec3f>& pts) {
    const HeadFrame& f = shell.frame();
    std::vector<float> shares;
    float s = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        s += (pts[i] - pts[i - 1]).length();
        if (s < 0.3f * f.radius) continue;
        const Vec3f q = pts[i] - f.center;
        const float r = q.length();
        if (r <= 0.0f) continue;
        const float t = shell.thickness(q);
        if (t < 0.02f * f.radius) continue;
        shares.push_back(std::clamp((r - shell.head(q)) / t, 0.0f, 1.2f));
    }
    if (shares.empty()) return 0.0f;
    std::nth_element(shares.begin(), shares.begin() + static_cast<std::ptrdiff_t>(shares.size() / 2), shares.end());
    return shares[shares.size() / 2];
}

}  // namespace

// ---- The field -------------------------------------------------------------------------------------------

GuideField::GuideField(const HairShell& shell, const std::vector<FieldGuide>& guides, const GuideFieldOptions& o,
                       const ScalpMesh* hairScalp)
    : shell_(&shell) {
    if (shell.empty()) throw std::invalid_argument("GuideField: no shell");
    const auto start = std::chrono::steady_clock::now();
    const HeadFrame& f = shell.frame();
    const float R = f.radius;
    // The box: the shell's reach round the head's middle, and the guides.
    Vec3f lo = f.center - Vec3f(1, 1, 1) * shell.maxOuter(), hi = f.center + Vec3f(1, 1, 1) * shell.maxOuter();
    for (const FieldGuide& g : guides)
        for (const Vec3f& p : g.points) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    float cell = o.cell > 0.0f ? o.cell : R / 14.0f;
    const float side = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    cell = std::max(cell, side / static_cast<float>(std::max(8, o.maxCells)));
    cell_ = cell;
    lo -= Vec3f(1, 1, 1) * (2.0f * cell);
    hi += Vec3f(1, 1, 1) * (2.0f * cell);
    origin_ = lo;
    nx_ = static_cast<int>(std::ceil((hi.x - lo.x) / cell));
    ny_ = static_cast<int>(std::ceil((hi.y - lo.y) / cell));
    nz_ = static_cast<int>(std::ceil((hi.z - lo.z) / cell));
    stats_.nx = nx_, stats_.ny = ny_, stats_.nz = nz_, stats_.cell = cell;
    const std::size_t total = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_) * static_cast<std::size_t>(nz_);
    auto centerOf = [&](std::size_t c) {
        const int i = static_cast<int>(c % static_cast<std::size_t>(nx_));
        const int j = static_cast<int>((c / static_cast<std::size_t>(nx_)) % static_cast<std::size_t>(ny_));
        const int k = static_cast<int>(c / (static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_)));
        return origin_ + Vec3f((static_cast<float>(i) + 0.5f) * cell, (static_cast<float>(j) + 0.5f) * cell, (static_cast<float>(k) + 0.5f) * cell);
    };
    auto cellAt = [&](int i, int j, int k) -> long long {
        if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) return -1;
        return (static_cast<long long>(k) * ny_ + j) * nx_ + i;
    };
    // The volume: between the head (its outermost half cell in, the scalp's cells) and the hull (half
    // a cell out), less what a photo shows bare; how far each is off the head (cells).
    std::vector<std::uint8_t> in(total, 0);
    std::vector<float> offHead(total, 0.0f);
    const bool photos = shell.photoCount() > 0;
    parallelFor(0, total, [&](std::size_t c) {
        const Vec3f p = centerOf(c);
        const Vec3f q = p - f.center;
        const float r = q.length();
        if (r < 1e-6f * R) return;
        const Vec3f u = q / r;
        const float h = shell.head(u);
        offHead[c] = (r - h) / cell;
        if (r < h - 0.5f * cell || r > shell.outer(u) + 0.5f * cell) return;
        if (photos && r > h && shell.seenBare(p)) return;
        in[c] = 1;
    }, 1024);
    // The guides' constraints: tangents half a cell apart (and past the tip, fading), splatted
    // trilinearly; the cells they touch are in the volume (a guide drawn a little outside the hull
    // still counts), unless inside the head.
    std::vector<float> wc(total, 0.0f);
    std::vector<Vec3f> wcv(total);
    const float lambda2 = o.lambda * o.lambda;
    auto splat = [&](const Vec3f& p, const Vec3f& value, float weight, bool force) {
        const Vec3f g = (p - origin_) / cell - Vec3f(0.5f, 0.5f, 0.5f);
        const int i0 = static_cast<int>(std::floor(g.x)), j0 = static_cast<int>(std::floor(g.y)), k0 = static_cast<int>(std::floor(g.z));
        const float fx = g.x - static_cast<float>(i0), fy = g.y - static_cast<float>(j0), fz = g.z - static_cast<float>(k0);
        for (int c = 0; c < 8; ++c) {
            const long long id = cellAt(i0 + (c & 1), j0 + ((c >> 1) & 1), k0 + ((c >> 2) & 1));
            if (id < 0) continue;
            const std::size_t s = static_cast<std::size_t>(id);
            if (!in[s]) {
                if (!force || offHead[s] < -0.5f) continue;
                in[s] = 1;
            }
            const float w = ((c & 1) ? fx : 1.0f - fx) * (((c >> 1) & 1) ? fy : 1.0f - fy) * (((c >> 2) & 1) ? fz : 1.0f - fz) * weight;
            if (w <= 0.0f) continue;
            wc[s] += lambda2 * w;
            wcv[s] += value * (lambda2 * w);
        }
    };
    const float stepAlong = 0.5f * cell;
    for (const FieldGuide& g : guides) {
        if (g.points.size() < 2 || !(g.weight > 0.0f)) continue;
        walk(g.points, stepAlong, [&](const Vec3f& p, const Vec3f& t, float) { splat(p, t, g.weight, true); });
        // The tip carried on along its last cell's direction, fading 1 to 0.
        const Vec3f tip = g.points.back();
        Vec3f back = tip;
        for (std::size_t i = g.points.size() - 1; i-- > 0;) {
            back = g.points[i];
            if ((tip - back).length() >= cell) break;
        }
        const Vec3f t = (tip - back).normalized();
        const float fade = std::max(0.0f, o.tipFade) * cell;
        if (t.length() > 0.5f && fade > 0.0f)
            for (float s = stepAlong; s <= fade; s += stepAlong) splat(tip + t * s, t * (1.0f - s / fade), g.weight, false);
    }
    // The unknowns.
    index_.assign(total, -1);
    std::vector<std::size_t> cells;
    for (std::size_t c = 0; c < total; ++c)
        if (in[c]) {
            index_[c] = static_cast<int>(cells.size());
            cells.push_back(c);
        }
    const std::size_t n = cells.size();
    stats_.cells = static_cast<int>(n);
    if (n == 0) {
        stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return;
    }
    // Neighbours, cut across the part over the top and front of the head.
    System A;
    A.nb.resize(n);
    A.diag.assign(n, 0.0f);
    auto partSide = [&](const Vec3f& p) { return (p - o.part.point).dot(o.part.normal) >= 0.0f; };
    auto partSeam = [&](const Vec3f& a, const Vec3f& b) {
        if (!o.part.on || partSide(a) == partSide(b)) return false;
        const Vec3f l = f.local(((a + b) * 0.5f - f.center).normalized());
        return l.y > 0.2f && l.z > -0.25f;
    };
    int seams = 0;
    for (std::size_t u = 0; u < n; ++u) {
        const std::size_t c = cells[u];
        const int i = static_cast<int>(c % static_cast<std::size_t>(nx_));
        const int j = static_cast<int>((c / static_cast<std::size_t>(nx_)) % static_cast<std::size_t>(ny_));
        const int k = static_cast<int>(c / (static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_)));
        const std::array<std::array<int, 3>, 6> off{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        int deg = 0;
        for (int e = 0; e < 6; ++e) {
            const long long id = cellAt(i + off[static_cast<std::size_t>(e)][0], j + off[static_cast<std::size_t>(e)][1], k + off[static_cast<std::size_t>(e)][2]);
            int v = id >= 0 ? index_[static_cast<std::size_t>(id)] : -1;
            if (v >= 0 && partSeam(centerOf(c), centerOf(static_cast<std::size_t>(id)))) {
                v = -1;
                ++seams;
            }
            A.nb[u][static_cast<std::size_t>(e)] = v;
            if (v >= 0) ++deg;
        }
        A.diag[u] = static_cast<float>(deg) + 1e-4f;
    }
    stats_.partSeams = seams / 2;
    // The scalp's cells (the first layer off the head, over where hair grows) and the ring.
    const Vec3f down = -f.up;
    std::vector<Vec3f> pore(n);
    std::vector<std::uint8_t> scalpCell(n, 0), ring(n, 0);
    parallelFor(0, n, [&](std::size_t u) {
        const std::size_t c = cells[u];
        const float off = offHead[c];
        if (off > o.ringCells) return;
        const Vec3f p = centerOf(c);
        Vec3f nrm = (p - f.center).normalized();
        if (hairScalp) {
            const SurfacePoint sp = hairScalp->closestPoint(p, 1.5f * cell + 0.02f * R);
            if (sp.root.face < 0) return;
            if (sp.normal.length() > 0.5f) nrm = sp.normal.dot(nrm) < 0.0f ? -sp.normal.normalized() : sp.normal.normalized();
        }
        ring[u] = 1;
        if (off <= 0.75f) scalpCell[u] = 1;
        pore[u] = (nrm + down * std::min(nrm.dot(down) + 1.0f, 1.0f)).normalized();
    }, 512);
    // The right-hand side and diagonal of the guides' constraints.
    std::vector<Vec3f> bGuides(n);
    std::vector<float> wGuides(n, 0.0f);
    for (std::size_t u = 0; u < n; ++u) {
        wGuides[u] = wc[cells[u]];
        bGuides[u] = wcv[cells[u]];
        if (wGuides[u] > 0.0f) ++stats_.guideCells;
    }
    // First solve: the guides, the pore direction held weakly (weight 1) at the scalp's cells.
    System A1 = A;
    std::vector<Vec3f> b(n), x(n);
    for (std::size_t u = 0; u < n; ++u) {
        A1.diag[u] += wGuides[u];
        b[u] = bGuides[u];
        if (scalpCell[u]) {
            A1.diag[u] += kPoreWeight;
            b[u] += pore[u] * (o.scalpMagnitude * kPoreWeight);
        }
        x[u] = b[u] / A1.diag[u];
    }
    auto [it1, res1] = solve(A1, b, x, o.maxIterations, o.tolerance);
    // Second solve: the near-scalp ring held (lambda^2) at a weak direction: the first field's with its
    // inward part removed, leaned toward the pore direction.
    System A2 = A;
    for (std::size_t u = 0; u < n; ++u) {
        A2.diag[u] += wGuides[u];
        b[u] = bGuides[u];
        if (!ring[u]) continue;
        ++stats_.ringCells;
        const Vec3f p = centerOf(cells[u]);
        const Vec3f nrm = (p - f.center).normalized();
        Vec3f d = x[u].normalized();
        if (d.dot(nrm) < 0.0f) d = (d - nrm * d.dot(nrm)).normalized();
        d = (d + pore[u] * o.poreLean).normalized();
        A2.diag[u] += lambda2;
        b[u] += d * (o.scalpMagnitude * lambda2);
    }
    auto [it2, res2] = solve(A2, b, x, o.maxIterations, o.tolerance);
    field_ = std::move(x);
    stats_.iterations = it1 + it2;
    stats_.residual = std::max(res1, res2);
    stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Vec3f GuideField::at(const Vec3f& p, float* inside) const {
    if (inside) *inside = 0.0f;
    if (field_.empty()) return {};
    const Vec3f g = (p - origin_) / cell_ - Vec3f(0.5f, 0.5f, 0.5f);
    const int i0 = static_cast<int>(std::floor(g.x)), j0 = static_cast<int>(std::floor(g.y)), k0 = static_cast<int>(std::floor(g.z));
    const float fx = g.x - static_cast<float>(i0), fy = g.y - static_cast<float>(j0), fz = g.z - static_cast<float>(k0);
    Vec3f v;
    float sw = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const int i = i0 + (c & 1), j = j0 + ((c >> 1) & 1), k = k0 + ((c >> 2) & 1);
        if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) continue;
        const int u = index_[(static_cast<std::size_t>(k) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(nx_) +
                             static_cast<std::size_t>(i)];
        if (u < 0) continue;
        const float w = ((c & 1) ? fx : 1.0f - fx) * (((c >> 1) & 1) ? fy : 1.0f - fy) * (((c >> 2) & 1) ? fz : 1.0f - fz);
        v += field_[static_cast<std::size_t>(u)] * w;
        sw += w;
    }
    if (inside) *inside = sw;
    return sw > 1e-6f ? v / sw : Vec3f{};
}

GrownGuide GuideField::grow(const Vec3f& root, float maxLength, const FieldGrowOptions& o) const {
    GrownGuide out;
    out.points.push_back(root);
    if (!shell_ || field_.empty() || !(maxLength > 0.0f)) {
        out.stop = GrowStop::Faded;
        return out;
    }
    const HeadFrame& f = shell_->frame();
    const float R = f.radius;
    const float step = std::max(0.05f, o.step) * cell_;
    const float keepOut = o.keepOut > 0.0f ? o.keepOut : 0.02f * R;
    const float cosAverage = std::cos(o.averageTurnDegrees * kPi / 180.0f), cosStop = std::cos(o.stopTurnDegrees * kPi / 180.0f);
    std::vector<Vec3f> pts{root};
    Vec3f p = root, prev;
    float s = 0.0f;
    {
        // A root under an overhang of the head (behind an ear: the head's radius that way is the ear's)
        // has no cells round it: it starts straight out from the head's middle, at the head's surface.
        float inside = 0.0f;
        at(p, &inside);
        const Vec3f q = p - f.center;
        const float r = q.length();
        if (inside < 0.25f && r > 0.0f) {
            const Vec3f lifted = f.center + q / r * std::max(r, shell_->head(q) + 0.5f * cell_);
            at(lifted, &inside);
            if (inside >= 0.25f && (lifted - p).length() > 1e-6f * R) {
                s = (lifted - p).length();
                p = lifted;
                pts.push_back(p);
            }
        }
    }
    out.stop = GrowStop::Length;
    const int maxSteps = static_cast<int>(std::ceil(maxLength / step)) + 1;
    for (int k = 0; k < maxSteps; ++k) {
        float inside = 0.0f;
        const Vec3f v = at(p, &inside);
        // (the root's cells straddle the head: the volume is only checked once off it)
        if (k > 1 && inside < 0.5f) {
            out.stop = GrowStop::Hull;
            break;
        }
        const float mag = v.length();
        if (mag < o.stopMagnitude) {
            out.stop = GrowStop::Faded;
            break;
        }
        Vec3f d = v / mag;
        if (k > 0) {
            const float c = d.dot(prev);
            if (c < cosStop) {
                out.stop = GrowStop::Turn;
                break;
            }
            if (c < cosAverage) d = (d + prev).normalized();
        }
        const float len = std::min(step, maxLength - s);
        if (len <= 1e-6f * R) break;
        if (o.ride > 0.0f) {
            // Steered toward its riding height in the shell (reached over the first 0.3 head radii).
            const Vec3f qc = p - f.center;
            const float rc = qc.length();
            if (rc > 0.0f) {
                const Vec3f u = qc / rc;
                const float want = shell_->head(u) + o.ride * shell_->thickness(u) * std::min(1.0f, s / (0.3f * R));
                d = (d + u * (o.rideGain * (want - rc) / R)).normalized();
            }
        }
        Vec3f q = p + d * len;
        // Kept off the head (ramped in over the first tenth of a head radius).
        const Vec3f r = q - f.center;
        const float rl = r.length();
        const float floorR = shell_->head(r) + keepOut * std::min(1.0f, (s + len) / (0.1f * R));
        if (rl > 0.0f && rl < floorR) q = f.center + r / rl * floorR;
        const Vec3f moved = q - p;
        if (moved.length() < 1e-6f * R) break;
        prev = moved.normalized();
        s += moved.length();
        p = q;
        pts.push_back(p);
        if (s >= maxLength - 1e-6f * R) break;
    }
    const float spacing = o.pointSpacing > 0.0f ? o.pointSpacing : R / 20.0f;
    out.points = pts.size() >= 2 ? resampledBySpacing(pts, spacing) : pts;
    out.points.front() = root;
    out.length = guideLength(out.points);
    return out;
}

// ---- The part --------------------------------------------------------------------------------------------

PartLine partFromGuides(const HeadFrame& f, const std::vector<std::vector<Vec3f>>& guides, int* votes) {
    const float R = f.radius;
    std::vector<std::pair<float, int>> v;  // root's offset along left, the way its first stretch heads (+1 left)
    for (const auto& g : guides) {
        if (g.size() < 2) continue;
        const Vec3f l = f.local((g.front() - f.center).normalized());
        if (l.y < 0.3f || l.z < -0.2f) continue;
        // Where it is 0.4 head radii along.
        Vec3f at = g.back();
        float s = 0.0f;
        for (std::size_t i = 1; i < g.size(); ++i) {
            s += (g[i] - g[i - 1]).length();
            if (s >= 0.4f * R) {
                at = g[i];
                break;
            }
        }
        const float lateral = (at - g.front()).dot(f.left);
        if (std::abs(lateral) < 0.12f * R) continue;
        v.push_back({(g.front() - f.center).dot(f.left), lateral > 0.0f ? 1 : -1});
    }
    if (votes) *votes = static_cast<int>(v.size());
    PartLine part;
    int lefts = 0, rights = 0;
    for (const auto& e : v) (e.second > 0 ? lefts : rights)++;
    if (lefts < 2 || rights < 2) return part;
    std::sort(v.begin(), v.end());
    // The offset t: guides heading left should be rooted left of it (offset > t), the rest right.
    int best = std::numeric_limits<int>::max();
    float bestT = 0.0f;
    for (std::size_t s = 0; s <= v.size(); ++s) {
        const float t = s == 0 ? v.front().first - 0.01f * R : s == v.size() ? v.back().first + 0.01f * R : 0.5f * (v[s - 1].first + v[s].first);
        int wrong = 0;
        for (const auto& e : v) wrong += (e.second > 0) != (e.first > t);
        if (wrong < best || (wrong == best && std::abs(t) < std::abs(bestT))) best = wrong, bestT = t;
    }
    if (static_cast<float>(best) > 0.2f * static_cast<float>(v.size()) || std::abs(bestT) > 0.8f * R) return part;
    part.on = true;
    part.point = f.center + f.left * bestT;
    part.normal = f.left;
    return part;
}

// ---- The fill -----------------------------------------------------------------------------------------------

FieldFill fieldFill(const std::vector<Vec3f>& roots, const std::vector<FieldGuide>& keys, const HairShell& shell, const FieldFillOptions& o,
                    const ScalpMesh* hairScalp) {
    if (shell.empty()) throw std::invalid_argument("fieldFill: no shell");
    std::vector<const FieldGuide*> usable;
    for (const FieldGuide& k : keys)
        if (k.points.size() >= 2 && k.weight > 0.0f) usable.push_back(&k);
    if (usable.empty()) throw std::invalid_argument("fieldFill: no guides to fill between");
    const GuideField field(shell, keys, o.field, hairScalp);
    FieldFill out;
    out.field = field.stats();
    const float R = shell.frame().radius;
    std::vector<float> lengths, rides;
    for (const FieldGuide* k : usable) {
        lengths.push_back(guideLength(k->points));
        rides.push_back(rideShare(shell, k->points));
    }
    out.guides.resize(roots.size());
    std::vector<GrowStop> stops(roots.size(), GrowStop::Length);
    parallelFor(0, roots.size(), [&](std::size_t i) {
        // Its length: the nearest keys' (by root), 1 / d^2.
        std::vector<std::pair<float, std::size_t>> near;
        near.reserve(usable.size());
        for (std::size_t k = 0; k < usable.size(); ++k) near.push_back({(usable[k]->points.front() - roots[i]).length(), k});
        const std::size_t m = std::min(near.size(), static_cast<std::size_t>(std::max(1, o.nearest)));
        std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(m), near.end());
        double sw = 0.0, sl = 0.0, sr = 0.0;
        for (std::size_t q = 0; q < m; ++q) {
            const float d = near[q].first;
            const double w = usable[near[q].second]->weight / (static_cast<double>(d) * d + 0.0025 * R * R);
            sw += w;
            sl += w * lengths[near[q].second];
            sr += w * rides[near[q].second];
        }
        Rng rng(hashCombine(o.seed, static_cast<std::uint64_t>(i) * 13 + 5));
        const float cap = static_cast<float>(sl / sw) * (1.0f + o.lengthSlack) * (1.0f + o.lengthRandom * rng.uniform(-1.0f, 1.0f));
        FieldGrowOptions go = o.grow;
        if (o.ride) go.ride = std::clamp(static_cast<float>(sr / sw), 0.0f, o.maxRide);
        GrownGuide g = field.grow(roots[i], cap, go);
        stops[i] = g.stop;
        out.guides[i] = std::move(g.points);
    }, 8);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (out.guides[i].size() < 2) {
            ++out.empty;
            continue;
        }
        switch (stops[i]) {
            case GrowStop::Length: ++out.byLength; break;
            case GrowStop::Faded: ++out.byFade; break;
            case GrowStop::Hull: ++out.byHull; break;
            case GrowStop::Turn: ++out.byTurn; break;
        }
    }
    return out;
}

// ---- Detail on top --------------------------------------------------------------------------------------------

std::vector<Vec3f> wavedGuide(const std::vector<Vec3f>& pts, const HeadFrame& f, const CombOptions& o, std::uint64_t seed) {
    const std::size_t n = pts.size();
    if (n < 3) return pts;
    const float R = f.radius;
    Rng rng(hashCombine(o.seed, seed * 31 + 17));
    const float amp0 = o.amplitude >= 0.0f ? o.amplitude : 0.08f * R;
    const float wl0 = o.wavelength > 0.0f ? o.wavelength : 0.5f * R;
    const float flip = o.flip >= 0.0f ? o.flip : 0.12f * R;
    const float phase = (1.0f - std::clamp(o.coherence, 0.0f, 1.0f)) * rng.uniform(-kPi, kPi);
    const float amp = amp0 * rng.uniform(0.8f, 1.2f), wl = wl0 * rng.uniform(0.92f, 1.08f);
    std::vector<float> along(n, 0.0f);
    for (std::size_t i = 1; i < n; ++i) along[i] = along[i - 1] + (pts[i] - pts[i - 1]).length();
    const float total = std::max(along.back(), 1e-6f * R);
    std::vector<Vec3f> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float si = along[i];
        const Vec3f tan = (pts[std::min(i + 1, n - 1)] - pts[i > 0 ? i - 1 : 0]).normalized();
        const Vec3f upR = (pts[i] - f.center).normalized();
        const Vec3f across = tan.cross(upR).normalized();
        const float ramp = std::clamp((si - 0.2f * R) / (0.3f * R), 0.0f, 1.0f);
        Vec3f q = pts[i] + across * (amp * ramp * std::sin(2.0f * kPi * si / wl + phase));
        const float t = si / total;
        if (t > 0.78f) {
            const float k = (t - 0.78f) / 0.22f;
            q += upR * (flip * k * k) + f.up * (0.6f * flip * k * k * k);
        }
        out.push_back(q);
    }
    out.front() = pts.front();
    return out;
}

}  // namespace groomlab
