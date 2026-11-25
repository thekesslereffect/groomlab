// SPDX-License-Identifier: Apache-2.0
#include "groomlab/tube_press.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>

#include "groomlab/parallel.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.283185307179586f;

struct Box {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void grow(const Vec3f& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    bool contains(const Vec3f& p) const { return p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x <= hi.x && p.y <= hi.y && p.z <= hi.z; }
    bool overlaps(const Box& o) const {
        return lo.x <= o.hi.x && o.lo.x <= hi.x && lo.y <= o.hi.y && o.lo.y <= hi.y && lo.z <= o.hi.z && o.lo.z <= hi.z;
    }
};

// 1 at 0 down to 0 at 1, smoothly (a smoothstep's fall).
float easeOff(float x) {
    if (x <= 0.0f) return 1.0f;
    if (x >= 1.0f) return 0.0f;
    return 1.0f - x * x * (3.0f - 2.0f * x);
}

bool isGrid(const PressTube& t) {
    const std::size_t n = t.stations();
    return n >= 2 && t.around >= 3 && t.normals.size() == n && t.binormals.size() == n &&
           t.reach.size() == n * static_cast<std::size_t>(t.around);
}

// The farthest a station's ring reaches (scene units: its reach times the longer of its axes).
float mostReach(const PressTube& t, std::size_t station) {
    const auto a = static_cast<std::size_t>(t.around);
    return *std::max_element(t.reach.begin() + static_cast<std::ptrdiff_t>(station * a),
                             t.reach.begin() + static_cast<std::ptrdiff_t>((station + 1) * a)) *
           std::max(t.normals[station].length(), t.binormals[station].length());
}

// A tube as the press asks it: each segment's box (its stations grown by twice their reach), and
// all of them.
struct Axis {
    std::vector<Box> segments;
    Box box;
};

Axis axisOf(const PressTube& t) {
    Axis a;
    for (std::size_t j = 0; j + 1 < t.stations(); ++j) {
        const float grow = 2.0f * std::max(mostReach(t, j), mostReach(t, j + 1));
        const Vec3f g(grow, grow, grow);
        Box b;
        b.grow(t.centers[j] - g);
        b.grow(t.centers[j] + g);
        b.grow(t.centers[j + 1] - g);
        b.grow(t.centers[j + 1] + g);
        a.box.grow(b.lo);
        a.box.grow(b.hi);
        a.segments.push_back(b);
    }
    return a;
}

// The reach between stations j and j + 1 (s of the way) at turn `theta`, linearly both ways.
float reachAt(const PressTube& t, std::size_t j, float s, float theta) {
    const auto around = static_cast<std::size_t>(t.around);
    float f = theta / kTwoPi * static_cast<float>(t.around);
    f -= std::floor(f / static_cast<float>(t.around)) * static_cast<float>(t.around);
    const auto k0 = static_cast<std::size_t>(std::floor(f)) % around, k1 = (k0 + 1) % around;
    const float u = f - std::floor(f);
    const float* r0 = t.reach.data() + j * around;
    const float* r1 = r0 + around;
    const float a = r0[k0] + (r0[k1] - r0[k0]) * u, b = r1[k0] + (r1[k1] - r1[k0]) * u;
    return a + (b - a) * s;
}

constexpr float kFar = 1e9f;

// How deep `p` lies in the tube by segment j (stations j and j + 1): its distance from the
// segment's center line over the reach that way there (below 1 inside); kFar past an open end.
float segmentLevel(const PressTube& t, std::size_t j, const Vec3f& p) {
    const std::size_t last = t.stations() - 1;
    const Vec3f c0 = t.centers[j], d = t.centers[j + 1] - c0;
    const float l2 = d.dot(d);
    float s = l2 > 0.0f ? (p - c0).dot(d) / l2 : 0.0f;
    if ((j == 0 && s < 0.0f) || (j + 1 == last && s > 1.0f)) return kFar;  // past an end
    s = std::clamp(s, 0.0f, 1.0f);
    const Vec3f e = p - (c0 + d * s);
    // Where it lies in the ring's axes (each as long as the ray's unit: x along the normal, y along
    // the binormal; past a clamped end, how far beyond in their mean length).
    const Vec3f nrm = t.normals[j] * (1.0f - s) + t.normals[j + 1] * s;
    const Vec3f bin = t.binormals[j] * (1.0f - s) + t.binormals[j + 1] * s;
    const float n2 = nrm.dot(nrm), b2 = bin.dot(bin);
    if (!(n2 > 0.0f) || !(b2 > 0.0f)) return kFar;
    const float x = e.dot(nrm) / n2, y = e.dot(bin) / b2;
    const float beyond = l2 > 0.0f ? e.dot(d) / std::sqrt(l2) / (0.5f * (std::sqrt(n2) + std::sqrt(b2))) : 0.0f;
    const float r = reachAt(t, j, s, std::atan2(y, x));
    if (!(r > 0.0f)) return kFar;
    return std::sqrt(x * x + y * y + beyond * beyond) / r;
}

// The station whose center lies nearest `p`, searched from all of them.
std::size_t nearestStation(const PressTube& t, const Vec3f& p) {
    std::size_t best = 0;
    float least = 1e30f;
    for (std::size_t j = 0; j < t.stations(); ++j) {
        const Vec3f e = p - t.centers[j];
        if (e.dot(e) < least) least = e.dot(e), best = j;
    }
    return best;
}

// How deep `p` lies in the tube (segmentLevel by the segments either side of the station nearest
// it): `hint` the station to search from (a nearby point's), set to the one found. The search
// steps along the stations while they come nearer, so it finds the nearest where the curve passes
// once within reach of the point (a tube does not fold back on itself that tightly).
float level(const PressTube& t, const Vec3f& p, std::size_t& hint) {
    const std::size_t n = t.stations();
    const auto far2 = [&](std::size_t j) {
        const Vec3f e = p - t.centers[j];
        return e.dot(e);
    };
    std::size_t j = std::min(hint, n - 1);
    float at = far2(j);
    for (std::size_t step = 16;; step /= 2) {
        for (;;) {
            if (j + step < n) {
                const float d = far2(j + step);
                if (d < at) {
                    j += step, at = d;
                    continue;
                }
            }
            if (j >= step) {
                const float d = far2(j - step);
                if (d < at) {
                    j -= step, at = d;
                    continue;
                }
            }
            break;
        }
        if (step == 1) break;
    }
    hint = j;
    float best = kFar;
    if (j > 0) best = std::min(best, segmentLevel(t, j - 1, p));
    if (j + 1 < n) best = std::min(best, segmentLevel(t, j, p));
    return best;
}

}  // namespace

namespace {

// The tubes whose boxes meet each one's (grids only).
std::vector<std::vector<std::uint32_t>> nearTubes(const std::vector<Axis>& axes, const std::vector<char>& grid) {
    std::vector<std::vector<std::uint32_t>> near(axes.size());
    std::vector<std::uint32_t> order;
    for (std::size_t a = 0; a < axes.size(); ++a) {
        if (grid[a]) order.push_back(static_cast<std::uint32_t>(a));
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) { return axes[x].box.lo.x < axes[y].box.lo.x; });
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (std::size_t j = i + 1; j < order.size() && axes[order[j]].box.lo.x <= axes[order[i]].box.hi.x; ++j) {
            if (axes[order[i]].box.overlaps(axes[order[j]].box)) {
                near[order[i]].push_back(order[j]);
                near[order[j]].push_back(order[i]);
            }
        }
    }
    return near;
}

// A station's ring's mean reach, scene units (times its axes' mean length).
float meanReach(const PressTube& t, std::size_t station) {
    const auto a = static_cast<std::size_t>(t.around);
    float sum = 0.0f;
    for (std::size_t j = 0; j < a; ++j) sum += t.reach[station * a + j];
    return sum / static_cast<float>(a) * 0.5f * (t.normals[station].length() + t.binormals[station].length());
}

Vec3f tangentAt(const PressTube& t, std::size_t i) {
    const std::size_t n = t.stations();
    const Vec3f d = t.centers[std::min(i + 1, n - 1)] - t.centers[i > 0 ? i - 1 : 0];
    return d.length() > 0.0f ? d.normalized() : Vec3f{};
}

// Each station's shift: centers that pass closer than half their sections' reach (times
// `separate`) moved apart, each half the way, so one lies over the other instead of both being
// squashed to a sliver: across both courses where they cross (the side each already lies on; the
// first tube up, the other down where they meet dead on), straight apart where they run side by
// side. Eased out along each tube over `distance`, as the squash is.
std::vector<std::vector<Vec3f>> separateCenters(const std::vector<PressTube>& tubes, const std::vector<Axis>& axes,
                                                const std::vector<char>& grid, const std::vector<std::vector<std::uint32_t>>& near,
                                                const PressSettings& settings) {
    std::vector<std::vector<Vec3f>> shift(tubes.size());
    for (std::size_t a = 0; a < tubes.size(); ++a) shift[a].assign(tubes[a].stations(), Vec3f{});
    const float separate = std::clamp(settings.separate, 0.0f, 1.0f);
    if (!(separate > 0.0f)) return shift;
    parallelFor(0, tubes.size(), [&](std::size_t a) {
        if (!grid[a] || near[a].empty()) return;
        const PressTube& me = tubes[a];
        const std::size_t count = me.stations();
        std::vector<Vec3f> need(count, Vec3f{});
        // Per other tube near: at each station, the line apart (the same for both tubes) and how far
        // along it this center lies from the other's; then one side for the pair (the side this
        // center lies on where they come closest, weighted by how close; dead on: the first tube's
        // way), so a lock never swaps sides along its run.
        struct Near {
            Vec3f way;
            float along = 0.0f, least = 0.0f;
            bool close = false;
        };
        std::vector<Near> at(count);
        for (const std::uint32_t b : near[a]) {
            const PressTube& other = tubes[b];
            const Axis& axis = axes[b];
            const bool first = a < b;
            float lean = 0.0f;
            bool any = false;
            for (std::size_t i = 0; i < count; ++i) {
                at[i].close = false;
                const Vec3f c = me.centers[i], ta = tangentAt(me, i);
                if (!axis.box.contains(c)) continue;
                // The closest point of its center curve (within the segments' boxes, not past an end).
                float best = 1e30f, bestS = 0.0f;
                std::size_t bestJ = 0;
                for (std::size_t j = 0; j < axis.segments.size(); ++j) {
                    if (!axis.segments[j].contains(c)) continue;
                    const Vec3f c0 = other.centers[j], d = other.centers[j + 1] - c0;
                    const float l2 = d.dot(d);
                    float s = l2 > 0.0f ? (c - c0).dot(d) / l2 : 0.0f;
                    if ((j == 0 && s < 0.0f) || (j + 2 == other.stations() && s > 1.0f)) continue;
                    s = std::clamp(s, 0.0f, 1.0f);
                    const float dist = (c - (c0 + d * s)).length();
                    if (dist < best) best = dist, bestS = s, bestJ = j;
                }
                if (!(best < 1e29f)) continue;
                const float ra = meanReach(me, i);
                const float rb = meanReach(other, bestJ) * (1.0f - bestS) + meanReach(other, bestJ + 1) * bestS;
                const float least = 0.5f * (ra + rb) * separate;
                if (!(best < least)) continue;
                const Vec3f q = other.centers[bestJ] + (other.centers[bestJ + 1] - other.centers[bestJ]) * bestS;
                const Vec3f tb = (other.centers[bestJ + 1] - other.centers[bestJ]).normalized();
                const Vec3f e = c - q;
                // Crossing (at any angle): across both courses, the first's ta x tb. Side by side:
                // straight apart, from the first toward the other.
                Vec3f way = first ? ta.cross(tb) : tb.cross(ta);
                if (way.length() > 0.02f) {
                    way = way.normalized();
                } else {
                    way = e - ta * e.dot(ta);
                    if (!first) way = -way;
                    way = way.length() > 1e-6f * (ra + 1.0f) ? way.normalized() : (first ? me.normals[i] : other.normals[bestJ]);
                }
                at[i] = {way, e.dot(way), least, true};
                lean += at[i].along * (least - best);
                any = true;
            }
            if (!any) continue;
            const float side = lean > 0.0f ? 1.0f : lean < 0.0f ? -1.0f : (first ? 1.0f : -1.0f);
            for (std::size_t i = 0; i < count; ++i) {
                if (!at[i].close) continue;
                const float short_ = at[i].least - side * at[i].along;  // how far short of `least` on its side
                if (!(short_ > 0.0f)) continue;
                const Vec3f push = at[i].way * (side * 0.5f * short_);
                if (push.length() > need[i].length()) need[i] = push;
            }
        }
        // Eased out along the tube (each station shifted at least as a shift within the distance
        // along it, eased off there), then smoothed.
        double spacing = 0.0;
        for (std::size_t i = 0; i + 1 < count; ++i) spacing += static_cast<double>((me.centers[i + 1] - me.centers[i]).length());
        spacing /= static_cast<double>(count - 1);
        const float reach = spacing > 0.0 ? std::max(settings.distance, 0.0f) / static_cast<float>(spacing) : 0.0f;
        std::vector<Vec3f> eased = need;
        if (reach > 0.0f) {
            const int span = static_cast<int>(std::ceil(std::min(reach, static_cast<float>(count))));
            for (std::size_t i = 0; i < count; ++i) {
                Vec3f bestV = need[i];
                float bestL = need[i].length();
                for (int k = 1; k <= span; ++k) {
                    const float share = easeOff(static_cast<float>(k) / reach);
                    if (!(share > 0.0f)) break;
                    const auto uk = static_cast<std::size_t>(k);
                    for (const std::size_t o : {i >= uk ? i - uk : count, i + uk < count ? i + uk : count}) {
                        if (o >= count) continue;
                        const float l = need[o].length() * share;
                        if (l > bestL) bestL = l, bestV = need[o] * share;
                    }
                }
                eased[i] = bestV;
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<Vec3f> smoothed(count);
            for (std::size_t i = 0; i < count; ++i) {
                const Vec3f m = (eased[i > 0 ? i - 1 : i] + eased[i] * 2.0f + eased[i + 1 < count ? i + 1 : i]) * 0.25f;
                smoothed[i] = m.length() < need[i].length() ? need[i] : m;
            }
            eased.swap(smoothed);
        }
        shift[a] = std::move(eased);
    }, 1);
    return shift;
}

// Each ray's reach after the press (pressTubes, the centers where they lie), as a version 2 Braid
// presses its sections (operators/braid_op.cpp pressSections), on any tubes:
//   - A ray's limit: how far out along it its point still lies deeper in its own tube than in any
//     other, each in its own size (the surface between two tubes, which both end on), the clearance
//     short of it; a ray that meets nothing within kRoom of its reach is free.
//   - Free, a ring is as large as lets it hold its section's area (the hair a contact presses out of
//     it goes to its free sides: `volume` of the way), no more than kRoom past its reach; that
//     growth eased along the tube over `distance`.
//   - Then rounded: smoothed round the ring (`smoothness`: up to four passes) and along the tube,
//     never past a limit, so a contact is a flat facet that fades in and out, its edge a rounded
//     corner, and the tubes never reach into each other.
std::vector<std::vector<float>> pressReach(const std::vector<PressTube>& tubes, const PressSettings& settings) {
    constexpr float kRoom = 1.25f;   // the most a ring spreads past its reach where it is free
    constexpr float kLeast = 0.1f;   // (a ray never closes: two centers through one point)
    std::vector<std::vector<float>> result(tubes.size());
    for (std::size_t a = 0; a < tubes.size(); ++a) result[a] = tubes[a].reach;
    const float smooth = std::clamp(settings.smoothness, 0.0f, 1.0f);
    const float keep = std::clamp(settings.volume, 0.0f, 1.0f);
    const float clearance = std::max(settings.clearance, 0.0f);

    std::vector<Axis> axes(tubes.size());
    std::vector<char> grid(tubes.size(), 0);
    for (std::size_t a = 0; a < tubes.size(); ++a) {
        grid[a] = isGrid(tubes[a]) ? 1 : 0;
        if (grid[a]) axes[a] = axisOf(tubes[a]);
    }
    const std::vector<std::vector<std::uint32_t>> near = nearTubes(axes, grid);

    parallelFor(0, tubes.size(), [&](std::size_t a) {
        if (!grid[a] || near[a].empty()) return;
        const PressTube& me = tubes[a];
        const std::size_t count = me.stations(), sides = static_cast<std::size_t>(me.around);
        std::vector<float> cosines(sides), sines(sides);
        for (std::size_t j = 0; j < sides; ++j) {
            cosines[j] = std::cos(kTwoPi * static_cast<float>(j) / static_cast<float>(sides));
            sines[j] = std::sin(kTwoPi * static_cast<float>(j) / static_cast<float>(sides));
        }
        // Each ray's limit (as a reach: how far out from the center).
        std::vector<float> limit(count * sides);
        bool any = false;
        std::vector<std::size_t> hints(near[a].size());
        for (std::size_t i = 0; i < count; ++i) {
            // (each other tube searched from its station nearest this ring's center)
            for (std::size_t q = 0; q < near[a].size(); ++q) hints[q] = nearestStation(tubes[near[a][q]], me.centers[i]);
            for (std::size_t j = 0; j < sides; ++j) {
                const std::size_t at = i * sides + j;
                const float rho = me.reach[at];
                limit[at] = kRoom * rho;
                if (!(rho > 0.0f)) continue;
                const Vec3f out = me.normals[i] * cosines[j] + me.binormals[i] * sines[j];
                const Vec3f p = me.centers[i] + out * rho;
                const float lift = clearance / (rho * std::max(out.length(), 1e-12f));  // (the clearance, in its own depth)
                for (std::size_t q = 0; q < near[a].size(); ++q) {
                    const PressTube& other = tubes[near[a][q]];
                    const Axis& axis = axes[near[a][q]];
                    if (!axis.box.contains(p) && !axis.box.contains(p - out * (0.9f * rho)) &&
                        !axis.box.contains(p + out * ((kRoom - 1.0f) * rho))) {
                        continue;
                    }
                    std::size_t hint = hints[q];
                    // (t: how far in from its reach; gap falls as t grows, 0 where the two meet)
                    auto gap = [&](float t) { return (1.0f - t / rho) + lift - level(other, p - out * t, hint); };
                    float lo = -(kRoom - 1.0f) * rho, hi = (1.0f - kLeast) * rho;
                    if (gap(lo) < 0.0f) continue;  // (clear of it as far out as it could go)
                    if (gap(hi) > 0.0f) {
                        lo = hi;
                    } else {
                        for (int step = 0; step < 24 && hi - lo > 1e-5f * rho; ++step) {
                            const float mid = 0.5f * (lo + hi);
                            if (gap(mid) > 0.0f) lo = mid;
                            else hi = mid;
                        }
                    }
                    limit[at] = std::min(limit[at], rho - 0.5f * (lo + hi));
                }
                any = any || limit[at] < rho;
            }
        }
        if (!any) return;

        // Free, each ring as large as holds its area (`volume` of the way), at most kRoom.
        std::vector<float> grow(count, 1.0f);
        if (keep > 0.0f) {
            for (std::size_t i = 0; i < count; ++i) {
                float area = 0.0f, held = 0.0f;
                for (std::size_t j = 0; j < sides; ++j) {
                    const float rho = me.reach[i * sides + j];
                    area += rho * rho;
                    held += std::min(limit[i * sides + j], rho) * std::min(limit[i * sides + j], rho);
                }
                if (!(held < area)) continue;
                float low = 1.0f, high = kRoom;
                for (int halve = 0; halve < 20; ++halve) {
                    const float m = 0.5f * (low + high);
                    float now = 0.0f;
                    for (std::size_t j = 0; j < sides; ++j) {
                        const float r = std::min(limit[i * sides + j], m * me.reach[i * sides + j]);
                        now += r * r;
                    }
                    if (now > area) high = m;
                    else low = m;
                }
                grow[i] = 1.0f + (0.5f * (low + high) - 1.0f) * keep;
            }
        }
        // ... eased along the tube (a contact's coming and going swells it gently): over `distance`.
        double spacing = 0.0;
        for (std::size_t i = 0; i + 1 < count; ++i) spacing += static_cast<double>((me.centers[i + 1] - me.centers[i]).length());
        spacing /= static_cast<double>(count - 1);
        const int along = spacing > 0.0 ? static_cast<int>(std::lround(std::max(settings.distance, 0.0f) / static_cast<float>(spacing))) : 0;
        const auto clampAt = [count](std::ptrdiff_t i) {
            return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(count) - 1));
        };
        for (int stride = 1; stride <= std::max(along / 2, 0); stride *= 2) {
            std::vector<float> eased(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto ii = static_cast<std::ptrdiff_t>(i);
                eased[i] = 0.25f * grow[clampAt(ii - stride)] + 0.5f * grow[i] + 0.25f * grow[clampAt(ii + stride)];
            }
            grow.swap(eased);
        }
        std::vector<float> scale(count * sides), work(count * sides);
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t j = 0; j < sides; ++j) scale[i * sides + j] = std::min(limit[i * sides + j], grow[i] * me.reach[i * sides + j]);
        }
        // ... and rounded: round the ring and along the tube, never past a limit.
        const int passes = static_cast<int>(std::lround(4.0f * smooth));
        for (int pass = 0; pass < passes; ++pass) {
            for (std::size_t i = 0; i < count; ++i) {
                const float* s = scale.data() + i * sides;
                float* to = work.data() + i * sides;
                for (std::size_t j = 0; j < sides; ++j) {
                    to[j] = (s[(j + sides - 2) % sides] + 4.0f * s[(j + sides - 1) % sides] + 6.0f * s[j] + 4.0f * s[(j + 1) % sides] +
                             s[(j + 2) % sides]) / 16.0f;
                }
            }
            for (int stride = 1; stride <= std::max(along / 4, 1); stride *= 2) {
                for (std::size_t i = 0; i < count; ++i) {
                    const auto ii = static_cast<std::ptrdiff_t>(i);
                    const float* before = work.data() + clampAt(ii - stride) * sides;
                    const float* here = work.data() + i * sides;
                    const float* after = work.data() + clampAt(ii + stride) * sides;
                    float* to = scale.data() + i * sides;
                    for (std::size_t j = 0; j < sides; ++j) to[j] = 0.25f * before[j] + 0.5f * here[j] + 0.25f * after[j];
                }
                work.swap(scale);
            }
            for (std::size_t q = 0; q < scale.size(); ++q) scale[q] = std::max(std::min(work[q], limit[q]), kLeast * me.reach[q]);
        }
        std::vector<float>& to = result[a];
        for (std::size_t q = 0; q < to.size(); ++q) {
            if (me.reach[q] > 0.0f) to[q] = scale[q];
        }
    }, 1);
    return result;
}

}  // namespace

PressResult pressTubes(const std::vector<PressTube>& tubes, const PressSettings& settings) {
    PressResult out;
    std::vector<Axis> axes(tubes.size());
    std::vector<char> grid(tubes.size(), 0);
    for (std::size_t a = 0; a < tubes.size(); ++a) {
        grid[a] = isGrid(tubes[a]) ? 1 : 0;
        if (grid[a]) axes[a] = axisOf(tubes[a]);
    }
    out.shift = separateCenters(tubes, axes, grid, nearTubes(axes, grid), settings);
    bool shifted = false;
    for (const auto& s : out.shift) {
        for (const Vec3f& v : s) shifted = shifted || v.dot(v) > 0.0f;
    }
    if (!shifted) {
        out.reach = pressReach(tubes, settings);
        return out;
    }
    std::vector<PressTube> moved = tubes;
    for (std::size_t a = 0; a < moved.size(); ++a) {
        for (std::size_t i = 0; i < moved[a].stations(); ++i) moved[a].centers[i] += out.shift[a][i];
    }
    out.reach = pressReach(moved, settings);
    return out;
}

namespace {

// The 2D convex hull of `points` (counter-clockwise, Andrew's monotone chain; fewer than 3 apart:
// what there is).
std::vector<std::array<float, 2>> convexHull(std::vector<std::array<float, 2>> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return points;
    const auto cross = [](const std::array<float, 2>& o, const std::array<float, 2>& a, const std::array<float, 2>& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    std::vector<std::array<float, 2>> hull(2 * points.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) --k;
        hull[k++] = points[i];
    }
    for (std::size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) --k;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

// How far out along (cos, sin) from the origin the hull reaches (0 when the ray misses it).
float rayReach(const std::vector<std::array<float, 2>>& hull, float c, float s) {
    float best = 0.0f;
    const std::size_t n = hull.size();
    if (n < 3) {
        for (const auto& p : hull) best = std::max(best, p[0] * c + p[1] * s);
        return best;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = hull[i];
        const auto& b = hull[(i + 1) % n];
        const float ex = b[0] - a[0], ey = b[1] - a[1];
        const float denom = c * ey - s * ex;  // the ray t (c, s) meets a + u e
        if (std::abs(denom) < 1e-12f) continue;
        const float t = (a[0] * ey - a[1] * ex) / denom;
        const float u = (a[0] * s - a[1] * c) / denom;
        if (u >= -1e-6f && u <= 1.0f + 1e-6f && t > best) best = t;
    }
    return best;
}

}  // namespace

std::shared_ptr<const StrandSet> pressLocks(const std::shared_ptr<const StrandSet>& strands, const std::vector<float>& lockOf,
                                            const PressSettings& settings, const std::function<float(std::size_t, float)>& amount) {
    if (!strands || strands->empty() || lockOf.size() != strands->strandCount()) return strands;
    const StrandSet& in = *strands;
    // The locks and their strands (each at least 2 points).
    std::vector<std::vector<std::size_t>> locks;
    {
        std::map<long, std::size_t> index;
        for (std::size_t s = 0; s < in.strandCount(); ++s) {
            const float v = lockOf[s];
            if (!(v >= 0.0f) || in.strandPointCount(s) < 2) continue;
            const auto [it, added] = index.emplace(std::lround(v), locks.size());
            if (added) locks.emplace_back();
            locks[it->second].push_back(s);
        }
    }
    if (locks.size() < 2) return strands;

    const std::vector<Vec3f>& at = in.positions();
    const bool widths = in.widths().size() == in.pointCount();
    // Each point's share of its strand's length.
    std::vector<float> share(in.pointCount(), 0.0f);
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        const std::size_t n = in.strandPointCount(s), b = in.strandBegin(s);
        if (n < 2) return;
        float total = 0.0f;
        for (std::size_t i = 1; i < n; ++i) {
            total += (at[b + i] - at[b + i - 1]).length();
            share[b + i] = total;
        }
        for (std::size_t i = 1; i < n; ++i) share[b + i] = total > 0.0f ? share[b + i] / total : static_cast<float>(i) / static_cast<float>(n - 1);
    });
    // A strand's point at share f of its length.
    const auto sample = [&](std::size_t s, float f) {
        const std::size_t n = in.strandPointCount(s), b = in.strandBegin(s);
        const float* sh = share.data() + b;
        const std::size_t i = static_cast<std::size_t>(std::upper_bound(sh + 1, sh + n, f) - sh);
        if (i >= n) return at[b + n - 1];
        const float span = sh[i] - sh[i - 1], u = span > 0.0f ? (f - sh[i - 1]) / span : 0.0f;
        return at[b + i - 1] + (at[b + i] - at[b + i - 1]) * u;
    };

    constexpr int kAround = 24;
    std::vector<float> cosines(kAround), sines(kAround);
    for (int j = 0; j < kAround; ++j) {
        cosines[static_cast<std::size_t>(j)] = std::cos(kTwoPi * static_cast<float>(j) / kAround);
        sines[static_cast<std::size_t>(j)] = std::sin(kTwoPi * static_cast<float>(j) / kAround);
    }
    // A point of a lock: its stations (k0 and k0 + 1, u of the way) and where it lies in their frame.
    struct Place {
        std::size_t k0 = 0;
        float u = 0.0f, x = 0.0f, y = 0.0f;
        Vec3f normal, binormal;
    };
    const auto placeOf = [](const PressTube& t, float f, const Vec3f& p) {
        Place q;
        const float g = f * static_cast<float>(t.stations() - 1);
        q.k0 = std::min(static_cast<std::size_t>(std::max(g, 0.0f)), t.stations() - 2);
        q.u = std::clamp(g - static_cast<float>(q.k0), 0.0f, 1.0f);
        const Vec3f c = t.centers[q.k0] + (t.centers[q.k0 + 1] - t.centers[q.k0]) * q.u;
        q.normal = t.normals[q.k0] * (1.0f - q.u) + t.normals[q.k0 + 1] * q.u;
        q.binormal = t.binormals[q.k0] * (1.0f - q.u) + t.binormals[q.k0 + 1] * q.u;
        const Vec3f e = p - c;
        q.x = e.dot(q.normal);
        q.y = e.dot(q.binormal);
        return q;
    };

    // Each lock as a tube.
    std::vector<PressTube> tubes(locks.size());
    parallelFor(0, locks.size(), [&](std::size_t l) {
        const std::vector<std::size_t>& members = locks[l];
        double mean = 0.0;
        for (const std::size_t s : members) mean += static_cast<double>(in.strandPointCount(s));
        mean /= static_cast<double>(members.size());
        const auto count = static_cast<std::size_t>(std::clamp<long>(std::lround(mean), 4, 48));
        PressTube& t = tubes[l];
        t.around = kAround;
        t.centers.assign(count, Vec3f{});
        for (const std::size_t s : members) {
            for (std::size_t k = 0; k < count; ++k) t.centers[k] += sample(s, static_cast<float>(k) / static_cast<float>(count - 1));
        }
        for (Vec3f& c : t.centers) c = c / static_cast<float>(members.size());
        // Frames carried along the center curve (each normal the last one squared to the course).
        t.normals.resize(count);
        t.binormals.resize(count);
        Vec3f last{};
        for (std::size_t k = 0; k < count; ++k) {
            Vec3f tangent = t.centers[std::min(k + 1, count - 1)] - t.centers[k > 0 ? k - 1 : 0];
            tangent = tangent.length() > 0.0f ? tangent.normalized() : Vec3f{0.0f, 1.0f, 0.0f};
            Vec3f n = last - tangent * last.dot(tangent);
            if (!(n.length() > 1e-4f)) {
                const Vec3f axis = std::abs(tangent.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f};
                n = axis - tangent * axis.dot(tangent);
            }
            n = n.normalized();
            t.normals[k] = n;
            t.binormals[k] = tangent.cross(n);
            last = n;
        }
        // Each station's outline: the hull round its hair there (each strand's point at the
        // station, and the points nearest it), plus the hair's half width.
        std::vector<std::vector<std::array<float, 2>>> near(count);
        float halfWidth = 0.0f;
        for (const std::size_t s : members) {
            for (std::size_t k = 0; k < count; ++k) {
                const Place q = placeOf(t, static_cast<float>(k) / static_cast<float>(count - 1),
                                        sample(s, static_cast<float>(k) / static_cast<float>(count - 1)));
                near[k].push_back({q.x, q.y});
            }
            const std::size_t n = in.strandPointCount(s), b = in.strandBegin(s);
            for (std::size_t i = 0; i < n; ++i) {
                const Place q = placeOf(t, share[b + i], at[b + i]);
                near[q.u < 0.5f ? q.k0 : q.k0 + 1].push_back({q.x, q.y});
                if (widths) halfWidth = std::max(halfWidth, 0.5f * in.widths()[b + i]);
            }
        }
        t.reach.assign(count * kAround, 0.0f);
        for (std::size_t k = 0; k < count; ++k) {
            const std::vector<std::array<float, 2>> hull = convexHull(std::move(near[k]));
            float most = 0.0f;
            for (int j = 0; j < kAround; ++j) {
                const float r = rayReach(hull, cosines[static_cast<std::size_t>(j)], sines[static_cast<std::size_t>(j)]);
                t.reach[k * kAround + static_cast<std::size_t>(j)] = r;
                most = std::max(most, r);
            }
            // (never thinner than a quarter of its widest: a flat or thin lock still has sides)
            for (int j = 0; j < kAround; ++j) {
                float& r = t.reach[k * kAround + static_cast<std::size_t>(j)];
                r = std::max(r, 0.25f * most) + halfWidth;
            }
        }
    }, 1);

    const PressResult pressed = pressTubes(tubes, settings);
    const std::vector<std::vector<float>>& pressedReach = pressed.reach;
    bool moved = false;
    for (std::size_t l = 0; l < locks.size() && !moved; ++l) {
        moved = pressedReach[l] != tubes[l].reach ||
                std::any_of(pressed.shift[l].begin(), pressed.shift[l].end(), [](const Vec3f& v) { return v.dot(v) > 0.0f; });
    }
    if (!moved) return strands;

    auto out = std::make_shared<StrandSet>(in);
    std::vector<Vec3f>& to = out->mutablePositions();
    parallelFor(0, locks.size(), [&](std::size_t l) {
        const PressTube& t = tubes[l];
        const std::vector<float>& now = pressedReach[l];
        const std::vector<Vec3f>& shift = pressed.shift[l];
        const auto around = static_cast<std::size_t>(t.around);
        for (const std::size_t s : locks[l]) {
            const std::size_t n = in.strandPointCount(s), b = in.strandBegin(s);
            for (std::size_t i = 0; i < n; ++i) {
                const float f = share[b + i];
                const float k = amount ? amount(s, f) : 1.0f;
                if (!(k > 0.0f)) continue;
                const Place q = placeOf(t, f, at[b + i]);
                // The ray it lies on, its scale: the pressed reach over the reach (bilinear).
                float a = std::atan2(q.y, q.x) / kTwoPi * static_cast<float>(t.around);
                a -= std::floor(a / static_cast<float>(t.around)) * static_cast<float>(t.around);
                const auto j0 = static_cast<std::size_t>(std::floor(a)) % around, j1 = (j0 + 1) % around;
                const float v = a - std::floor(a);
                const auto ratio = [&](std::size_t station, std::size_t j) {
                    const float r = t.reach[station * around + j];
                    return r > 0.0f ? now[station * around + j] / r : 1.0f;
                };
                const float r0 = ratio(q.k0, j0) + (ratio(q.k0, j1) - ratio(q.k0, j0)) * v;
                const float r1 = ratio(q.k0 + 1, j0) + (ratio(q.k0 + 1, j1) - ratio(q.k0 + 1, j0)) * v;
                const float scale = r0 + (r1 - r0) * q.u;
                const Vec3f across = q.normal * q.x + q.binormal * q.y;
                const Vec3f by = shift[q.k0] + (shift[q.k0 + 1] - shift[q.k0]) * q.u;
                to[b + i] = at[b + i] + (by + across * (scale - 1.0f)) * std::min(k, 1.0f);
            }
        }
    }, 1);
    return out;
}

}  // namespace groomlab
