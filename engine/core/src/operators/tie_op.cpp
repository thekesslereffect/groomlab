// SPDX-License-Identifier: Apache-2.0
// Tie: ponytails, hair ties and clips. The strands it affects (the common `affect`: a Group, a map
// or an expression picks the ponytail's hair) run from their roots to a ring (`center`, `radius`,
// facing `direction`), taut along the scalp, pass through it in the order they came from and fan
// out into a straight tail past it, every strand its own length.
//
// Each affected strand, from its start (its root; or, after an earlier Tie writing the same tail
// channel, the point where that Tie's tail starts: a second Tie down the tail of the first):
//
// 1. Where it passes the ring. A source frame: the origin O (the scalp's bounding-box center; for
//    a second Tie down a tail, the mean of the start points) and the pole P from O through the
//    center. Each start point S has an azimuth theta about that axis and a radial measure: the
//    chord |d - P| of its direction d from O (the Lambert azimuthal equal-area projection about the
//    pole: roots right under the tie 0, roots on the far side of the head 2), or its distance from
//    the axis for a second Tie. The radial measure is divided by the most any strand at that
//    azimuth has (64 bins of the core strands, each the greatest of its own and its neighbours',
//    interpolated: the same at every preview density), so the bundle fills the ring in every
//    direction. The ring point is at the same azimuth about the tie's axis (the source frame turned
//    from P onto `direction`), that share of kRingFill x radius out (wider below tightness 1).
//    The map keeps the azimuth and is monotonic in the radial measure, so strands keep their
//    arrangement through the ring: none crosses another.
// 2. The route to it, in the plane through the scalp's center C0, S and the ring point Q (for a first
//    Tie C0 is O, so the plane holds the axis: S and Q are at the same azimuth): the scalp's closest
//    points under the arc from S to Q about C0 (one per 1/32 of a half turn), lifted a clearance
//    (1/400 of the scalp's size) off the skin, then the outer convex hull of S, those and Q about C0
//    (a Graham scan): the taut string from S over the scalp to the ring, lying on the
//    skin where the skin is in the way and leaving it along the tangent toward the ring. Over its
//    first `ease` of length the route is blended from the strand's own shape (all of it at S, none
//    at `ease`), so roots leave the scalp the way they did and bend over smoothly.
// 3. The tail. From Q along `direction`, the strand's offset from the axis growing at tan(spread)
//    for the outermost strands (a cone; inner strands proportionally less) and turning `twist`
//    turns per unit length about the axis. The bend at the ring is rounded over kCorner x radius
//    on either side (a quadratic Bezier through Q at its middle), so the strand passes at Q itself.
// 4. The strand laid along that path. The whole path (route, bend and tail) is pushed out of the
//    scalp to the clearance (its collision field: distance_field.h), then each point after S is put
//    on it at exactly its segment's length from the point before (the first crossing of a sphere
//    of that radius with the path), so every segment, and so the strand, keeps its length exactly;
//    a path too short is carried on straight. Strands too short to reach the ring end on the way to
//    it (loose wisps lying along the scalp, never stretched).
//
// The per-point channel `tailChannel` (`tail`) goes from 0 to 1 over a radius either side of where
// a strand passes the ring (strands it leaves alone keep what they had, or 0), for nodes after it to
// work on the tail alone (Curl's affect mapped to it).
//
// Why the hull over the skin's closest points and not surface paths (surface_paths.h): the taut
// string a tie makes is a shortest path over the skin to a point off it, which near the tie leaves
// the skin; a front along the skin gives distances, not that path. In the plane through the axis
// the string is the outer hull of the skin's section, which costs a few closest-point queries a
// strand and is exact on a convex scalp; the collision field then keeps the rest of the path out.
//
// Reads other strands (the core's radial measures per azimuth): evaluates in full. The GPU preview
// runs it too (look.slang TieFrameCS, TieRouteCS, TieLayCS): the bins from the core strands it is
// handed, the routes' closest points down the scalp's field's mesh, the push through its tables. So
// does the CPU engine, there the core's queries themselves (look_run.h TieHost): the bits of the C++
// below, which runs a masked Tie and a scalp deformed away from its rest pose.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kBins = 64;                         // azimuth bins of the most radial measure
constexpr float kRingFill = 0.75f;                // the bundle's radius at tightness 1, a share of the ring's
constexpr float kCorner = 1.0f;                   // the bend at the ring rounded over this share of the radius, each side
constexpr float kClearanceShare = 1.0f / 400.0f;  // how far off the skin strands are kept, a share of the scalp's size
constexpr float kDepthShare = 1.0f / 8.0f;        // how deep under the skin a point is still pushed out
constexpr int kSkinSamplesPerHalfTurn = 32;       // closest points under the route's arc
constexpr std::size_t kMaxPath = 4096;            // the most points of a strand's path

// Two unit vectors perpendicular to unit n and to each other (a, b, n right-handed).
void basisAbout(const Vec3f& n, Vec3f* a, Vec3f* b) {
    const Vec3f ref = std::fabs(n.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
    *a = ref.cross(n).normalized();
    *b = n.cross(*a);
}

// v turned by the shortest turn taking unit `from` onto unit `to`.
Vec3f turnOnto(const Vec3f& v, const Vec3f& from, const Vec3f& to) {
    const Vec3f axis = from.cross(to);
    const float s = axis.length(), c = std::clamp(from.dot(to), -1.0f, 1.0f);
    if (s < 1e-7f) {
        if (c > 0.0f) return v;
        Vec3f a, b;  // opposite: half a turn about any perpendicular
        basisAbout(from, &a, &b);
        return rotateAxisAngle(v, a, kPi);
    }
    return rotateAxisAngle(v, axis / s, pm::atan2(s, c));
}

// Where along a polyline (points, cumulative lengths) arc length a falls; past its ends, along its
// first or last segment.
Vec3f alongPolyline(const std::vector<Vec3f>& pts, const std::vector<float>& arcs, float a) {
    const std::size_t n = pts.size();
    if (n == 1) return pts[0];
    std::size_t j = static_cast<std::size_t>(std::upper_bound(arcs.begin(), arcs.end(), a) - arcs.begin());
    j = std::clamp<std::size_t>(j, 1, n - 1);
    const float len = arcs[j] - arcs[j - 1];
    const float f = len > 0.0f ? (a - arcs[j - 1]) / len : 0.0f;
    return pts[j - 1] + (pts[j] - pts[j - 1]) * f;
}

// A point of a route's plane (x, y about the scalp's center) and where it is.
struct Planar {
    float x, y;
    Vec3f p;
};

class TieOp : public Operator {
public:
    std::string typeName() const override { return op::kTie; }
    std::string category() const override { return "shape"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"center", Vec3f{0.0f, 1.0f, -11.0f},
                      "Where the tie is (world space): put it a little off the head, where the ponytail is gathered "
                      "(Maya: Pick sets it from a selected vertex, locator or transform)",
                      "length"}
                .world(),
            ParamSpec{"radius", 1.2, "The tie's radius: the bundle passes within it", "length"}.atLeast(0).soft(0, 5),
            ParamSpec{"direction", Vec3f{0.0f, 0.0f, 0.0f},
                      "The way the tail leaves the tie (world space). (0, 0, 0): from the scalp's center through the "
                      "center (a second Tie down a tail: from the first tie through this one)"}
                .world(),
            ParamSpec{"tightness", 1.0,
                      "How tightly the tie gathers the strands, 0-1: 1 all within its radius, lower a looser, wider "
                      "bundle (up to twice as wide)",
                      "fraction"}
                .range(0, 1),
            ParamSpec{"spread", 10.0,
                      "How much the tail fans out past the tie: the outermost strands' angle off the tail's direction "
                      "(inner ones less), degrees",
                      "angle"}
                .range(0, 80)
                .soft(0, 45),
            ParamSpec{"twist", 0.0,
                      "Turns of the whole tail about its axis per unit length past the tie (negative the other way)",
                      "perLength"}
                .soft(-0.5, 0.5),
            ParamSpec{"ease", 0.5,
                      "Length over which each strand leaves its root its own way before it lies along the scalp toward "
                      "the tie, so roots do not kink (0: straight onto the route)",
                      "length"}
                .atLeast(0)
                .soft(0, 3),
            notAnimatable({"tailChannel", std::string("tail"),
                           "Per-point attribute receiving 0 before the tie and 1 past it (a smooth step over a radius "
                           "either side), for nodes after it to work on the tail alone (map their affect to it). A "
                           "second Tie writing the same attribute starts where the first one's tail does (a tie "
                           "further down the tail). Empty: none"})
                .adv()
                .writesChannel(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kTie);
        if (in->empty()) return in;
        const std::size_t strands = in->strandCount();
        const MeshPtr scalp = valueAs<MeshPtr>(ctx.external("scalp"));
        if (gpu::cpuEngineOn() && affectOf(*this) != AffectKind::Mask &&
            !(scalp && scalp->hasRest() && scalp->restPoints() != scalp->points())) {
            gpu::StageInputs stage;
            stage.tie = gpu::tieInputs(in, scalp, params().getString("tailChannel"));
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Tie, params(), in, scalp.get(), std::move(stage))) {
                // (Nothing tied: the strands as they came.)
                return std::memcmp(made->positions().data(), in->positions().data(), in->pointCount() * sizeof(Vec3f)) == 0 ? in : made;
            }
        }
        const bool hasScalp = scalp && scalp->triangleCount() > 0;

        const Vec3f center = params().getVec3("center");
        const auto radius = std::max(0.0f, static_cast<float>(params().getDouble("radius")));
        const auto tightness = std::clamp(static_cast<float>(params().getDouble("tightness")), 0.0f, 1.0f);
        const float spread = std::clamp(static_cast<float>(params().getDouble("spread")), 0.0f, 80.0f) * kPi / 180.0f;
        const auto twist = static_cast<float>(params().getDouble("twist"));
        const auto ease = std::max(0.0f, static_cast<float>(params().getDouble("ease")));
        const std::string channel = params().getString("tailChannel");
        const Vec3f directionParam = params().getVec3("direction");

        // Which strands it ties: those its affect reaches at all (the graph mixes the result back
        // toward the input by it afterwards).
        std::vector<char> tied(strands, 1);
        if (affectOf(*this) == AffectKind::Mask) {
            const ScalarField affect(params(), kAffectParam, scalp.get());
            affect.validate(*in);
            for (std::size_t s = 0; s < strands; ++s) tied[s] = affect.at(*in, s) > 0.0f ? 1 : 0;
        }
        // Where each strand starts: after an earlier tie's tail start (its tail channel at 0.5).
        const std::vector<float>* earlier = channel.empty() ? nullptr : in->pointChannel(channel);
        if (earlier && earlier->size() != in->pointCount()) earlier = nullptr;
        std::vector<std::size_t> start(strands, 0);
        for (std::size_t s = 0; s < strands; ++s) {
            const std::size_t n = in->strandPointCount(s);
            if (n < 2) {
                tied[s] = 0;
                continue;
            }
            if (earlier) {
                const float* t = earlier->data() + in->strandBegin(s);
                for (std::size_t i = 1; i < n; ++i) {
                    if (t[i] >= 0.5f) {
                        start[s] = i;
                        break;
                    }
                }
            }
            if (start[s] + 1 >= n) tied[s] = 0;  // its tail starts at its tip: nothing to tie
        }

        // The strands the shared measures read: the core's (the same at every preview density).
        const PreviewCore core(*in);
        std::vector<std::size_t> readers;
        for (std::size_t s = 0; s < strands; ++s) {
            if (tied[s] && core.core(s)) readers.push_back(s);
        }
        if (readers.empty()) {
            for (std::size_t s = 0; s < strands; ++s) {
                if (tied[s]) readers.push_back(s);
            }
        }
        if (readers.empty()) return in;  // nothing to tie: as it came

        // The source frame: a second tie down a tail measures its start points from their mean
        // (around the earlier tie), a first one its roots from the scalp's center.
        auto startOf = [&](std::size_t s) { return in->strandPoints(s)[start[s]]; };
        std::size_t chained = 0;
        double mx = 0.0, my = 0.0, mz = 0.0;
        for (const std::size_t s : readers) {
            chained += start[s] > 0 ? 1 : 0;
            const Vec3f p = startOf(s);
            mx += p.x;
            my += p.y;
            mz += p.z;
        }
        const double count = static_cast<double>(readers.size());
        const Vec3f mean{static_cast<float>(mx / count), static_cast<float>(my / count), static_cast<float>(mz / count)};
        const bool chain = 2 * chained > readers.size();
        Vec3f origin = mean;
        Vec3f scalpCenter = mean;  // what routes wrap about
        float scalpSize = 0.0f;
        if (hasScalp) {
            Vec3f lo = scalp->points().front(), hi = lo;
            for (const Vec3f& p : scalp->points()) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            scalpSize = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
            scalpCenter = (lo + hi) * 0.5f;
            if (!chain) origin = scalpCenter;
        }
        Vec3f pole = (center - origin).normalized();
        Vec3f axis = directionParam.normalized();
        if (pole.length() < 0.5f) pole = axis.length() > 0.5f ? axis : Vec3f{0.0f, -1.0f, 0.0f};
        if (axis.length() < 0.5f) axis = pole;  // auto: from the origin through the center
        Vec3f u1, u2;
        basisAbout(pole, &u1, &u2);
        const Vec3f e1 = turnOnto(u1, pole, axis), e2 = turnOnto(u2, pole, axis);

        // Each tied strand's azimuth and radial measure about the source axis.
        std::vector<float> theta(strands, 0.0f), radial(strands, 0.0f);
        parallelFor(0, strands, [&](std::size_t s) {
            if (!tied[s]) return;
            const Vec3f v = startOf(s) - origin;
            const float x = v.dot(u1), y = v.dot(u2);
            theta[s] = pm::atan2(y, x);
            if (chain) {
                radial[s] = std::sqrt(x * x + y * y);
            } else {
                const Vec3f d = v.normalized();
                radial[s] = d.length() > 0.5f ? (d - pole).length() : 0.0f;
            }
        });
        // The most radial measure per azimuth (the core's), each bin the greatest of itself and its
        // neighbours, interpolated between bin centers: no strand of the core is past it.
        std::array<float, kBins> most{};
        std::array<char, kBins> filled{};
        auto binOf = [](float angle) {
            const int k = static_cast<int>(std::floor((angle + kPi) / (2.0f * kPi) * static_cast<float>(kBins)));
            return ((k % kBins) + kBins) % kBins;
        };
        for (const std::size_t s : readers) {
            const int k = binOf(theta[s]);
            most[static_cast<std::size_t>(k)] = std::max(most[static_cast<std::size_t>(k)], radial[s]);
            filled[static_cast<std::size_t>(k)] = 1;
        }
        std::array<float, kBins> nearest = most;
        for (int k = 0; k < kBins; ++k) {  // an empty bin: the nearest filled one's
            if (filled[static_cast<std::size_t>(k)]) continue;
            for (int d = 1; d <= kBins / 2; ++d) {
                const int a = (k + d) % kBins, b = (k - d + kBins) % kBins;
                const bool fa = filled[static_cast<std::size_t>(a)], fb = filled[static_cast<std::size_t>(b)];
                if (!fa && !fb) continue;
                nearest[static_cast<std::size_t>(k)] =
                    std::max(fa ? most[static_cast<std::size_t>(a)] : 0.0f, fb ? most[static_cast<std::size_t>(b)] : 0.0f);
                break;
            }
        }
        std::array<float, kBins> reach{};
        for (int k = 0; k < kBins; ++k) {
            reach[static_cast<std::size_t>(k)] = std::max({nearest[static_cast<std::size_t>((k + kBins - 1) % kBins)],
                                                           nearest[static_cast<std::size_t>(k)],
                                                           nearest[static_cast<std::size_t>((k + 1) % kBins)]});
        }
        auto reachAt = [&](float angle) {
            const float f = (angle + kPi) / (2.0f * kPi) * static_cast<float>(kBins) - 0.5f;
            const float fl = std::floor(f);
            const int k0 = ((static_cast<int>(fl) % kBins) + kBins) % kBins;
            const int k1 = (k0 + 1) % kBins;
            const float w = f - fl;
            return reach[static_cast<std::size_t>(k0)] * (1.0f - w) + reach[static_cast<std::size_t>(k1)] * w;
        };

        const float bundle = kRingFill * radius * (2.0f - tightness);  // the bundle's radius at the ring
        const float tanSpread = pm::tan(spread);
        const float corner = kCorner * radius;
        const float clearance = hasScalp ? scalpSize * kClearanceShare : 0.0f;
        const float depth = hasScalp ? scalpSize * kDepthShare : 0.0f;
        const float searchRadius = std::max(clearance, depth) * 1.001f + 1e-6f;
        const CollisionSurface surface =
            hasScalp && clearance > 0.0f ? CollisionSurface::of(scalp, clearance, depth, searchRadius) : CollisionSurface{};

        auto out = std::make_shared<StrandSet>(*in);
        out->mutablePositions();  // cloned once here, not by whichever thread writes first
        float* tail = nullptr;
        if (!channel.empty()) {
            std::vector<float>* written = out->mutablePointChannel(channel);
            tail = written ? written->data() : out->addPointChannel(channel, 0.0f).data();
        }

        auto tie = [&](std::size_t s) {
            if (!tied[s]) return;
            const std::size_t n = in->strandPointCount(s), begin = in->strandBegin(s), k = start[s];
            const Vec3f* src = in->strandPoints(s);
            Vec3f* pts = out->strandPoints(s);
            thread_local std::vector<float> lengths, inputArcs, routeArcs, pathArcs;
            thread_local std::vector<Vec3f> inputPts, route, path;
            // The strand's segments from its start, and its shape there (for the ease).
            lengths.assign(n, 0.0f);
            inputPts.assign(src + k, src + n);
            inputArcs.assign(1, 0.0f);
            float remaining = 0.0f;
            for (std::size_t i = k + 1; i < n; ++i) {
                lengths[i] = (src[i] - src[i - 1]).length();
                remaining += lengths[i];
                inputArcs.push_back(remaining);
            }
            if (tail) {
                for (std::size_t i = begin; i <= begin + k; ++i) tail[i] = 0.0f;
            }
            if (!(remaining > 0.0f)) return;
            const Vec3f S = src[k];

            // Where it passes the ring.
            const float m = reachAt(theta[s]);
            const float share = m > 0.0f ? std::min(1.0f, radial[s] / m) : 0.0f;
            const Vec3f outward = e1 * pm::cos(theta[s]) + e2 * pm::sin(theta[s]);
            const Vec3f Q = center + outward * (share * bundle);

            // The route: the outer hull, about the scalp's center, of S, the scalp under the arc
            // and Q.
            route.assign(1, S);
            if (hasScalp) {
                const Vec3f a = S - scalpCenter, b = Q - scalpCenter;
                const float la = a.length(), lb = b.length();
                if (la > 1e-6f && lb > 1e-6f) {
                    const Vec3f ua = a / la, ub = b / lb;
                    const float cosA = std::clamp(ua.dot(ub), -1.0f, 1.0f);
                    const float alpha = pm::acos(cosA);
                    Vec3f w = ub - ua * cosA;
                    if (w.length() < 1e-4f) {  // Q straight over or under S: the plane through the axis
                        const Vec3f r = u1 * pm::cos(theta[s]) + u2 * pm::sin(theta[s]);
                        w = r - ua * ua.dot(r);
                    }
                    w = w.normalized();
                    const int samples = static_cast<int>(std::ceil(alpha / (kPi / static_cast<float>(kSkinSamplesPerHalfTurn))));
                    thread_local std::vector<std::pair<float, Planar>> skin;
                    skin.clear();
                    if (w.length() > 0.5f && alpha > 1e-4f) {
                        for (int j = 1; j < samples; ++j) {
                            const float f = static_cast<float>(j) / static_cast<float>(samples);
                            const float angle = alpha * f;
                            const Vec3f dir = ua * pm::cos(angle) + w * pm::sin(angle);
                            const float r = la + (lb - la) * f;
                            const SurfacePoint sp = scalp->closestPoint(scalpCenter + dir * r, 0.5f * r);
                            if (sp.root.face < 0) continue;
                            const Vec3f p = sp.position + sp.normal * clearance;
                            const Vec3f v = p - scalpCenter;
                            const float x = v.dot(ua), y = v.dot(w);
                            const float at = pm::atan2(y, x);
                            if (!(at > 0.0f && at < alpha)) continue;
                            skin.push_back({at, {x, y, p}});
                        }
                    }
                    std::stable_sort(skin.begin(), skin.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
                    thread_local std::vector<Planar> hull;
                    hull.assign(1, {la, 0.0f, S});
                    auto add = [&](const Planar& next) {
                        while (hull.size() >= 2) {
                            const Planar& p0 = hull[hull.size() - 2];
                            const Planar& p1 = hull.back();
                            const float cross = (p1.x - p0.x) * (next.y - p1.y) - (p1.y - p0.y) * (next.x - p1.x);
                            if (cross > 0.0f) break;  // p1 holds the string up: it bends over it
                            hull.pop_back();
                        }
                        hull.push_back(next);
                    };
                    for (const auto& sample : skin) add(sample.second);
                    add({lb * pm::cos(alpha), lb * pm::sin(alpha), Q});
                    for (std::size_t j = 1; j + 1 < hull.size(); ++j) route.push_back(hull[j].p);
                }
            }
            route.push_back(Q);
            routeArcs.assign(1, 0.0f);
            for (std::size_t j = 1; j < route.size(); ++j) routeArcs.push_back(routeArcs.back() + (route[j] - route[j - 1]).length());
            const float routeLength = routeArcs.back();

            // The path, sampled every `step`: the route (eased from the strand's own shape), the
            // bend through the ring, the tail.
            const float meanSegment = remaining / static_cast<float>(n - 1 - k);
            const float total = std::max(routeLength, remaining) + 2.0f * radius;
            float step = meanSegment / 3.0f;
            if (radius > 0.0f) step = std::min(step, radius / 3.0f);
            if (twist != 0.0f) step = std::min(step, 1.0f / (std::fabs(twist) * 24.0f));
            step = std::max(step, total / static_cast<float>(kMaxPath / 2));
            const float bendIn = std::min(corner, 0.5f * routeLength);
            path.clear();
            const float routeEnd = routeLength - bendIn;
            const int routeSteps = std::max(1, static_cast<int>(std::ceil(routeEnd / step)));
            for (int j = 0; j <= routeSteps; ++j) {
                const float a = routeEnd * static_cast<float>(j) / static_cast<float>(routeSteps);
                Vec3f p = alongPolyline(route, routeArcs, a);
                if (ease > 0.0f && a < ease && j > 0) {
                    const float keep = 1.0f - smoothstep(0.0f, ease, a);
                    p = p + (alongPolyline(inputPts, inputArcs, a) - p) * keep;
                }
                path.push_back(p);
            }
            const Vec3f bendStart = path.back();
            // The tail, by its distance t along the axis past the ring.
            const Vec3f offset = Q - center;
            auto tailAt = [&](float t) {
                Vec3f off = bundle > 0.0f ? offset * (1.0f + t * tanSpread / bundle) : offset;
                if (twist != 0.0f) off = rotateAxisAngle(off, axis, 2.0f * kPi * twist * t);
                return center + axis * t + off;
            };
            // Where the bend ends: kCorner x radius along the tail.
            float t = 0.0f, tailArc = 0.0f;
            Vec3f previous = Q;
            const float tStep = 0.5f * step;
            while (tailArc < corner) {
                const Vec3f next = tailAt(t + tStep);
                const float len = (next - previous).length();
                if (tailArc + len >= corner && len > 0.0f) {
                    const float f = (corner - tailArc) / len;
                    t += tStep * f;
                    tailArc = corner;
                    previous = tailAt(t);
                    break;
                }
                tailArc += len;
                t += tStep;
                previous = next;
            }
            const Vec3f bendEnd = corner > 0.0f ? previous : Q;
            const float bendLength = (Q - bendStart).length() + (bendEnd - Q).length();
            const int bendSteps = 2 * std::max(1, static_cast<int>(std::ceil(0.5f * bendLength / step)));  // even: Q a sample
            float ringArc = 0.0f;  // where along the path the strand passes the ring
            // A quadratic Bezier through Q at its middle (its control point 2Q - (start + end) / 2):
            // the strand passes the ring at Q itself, turning toward the tail as it goes through.
            const Vec3f control = Q * 2.0f - (bendStart + bendEnd) * 0.5f;
            for (int j = 1; j <= bendSteps; ++j) {
                const float f = static_cast<float>(j) / static_cast<float>(bendSteps);
                const float g = 1.0f - f;
                path.push_back(bendStart * (g * g) + control * (2.0f * g * f) + bendEnd * (f * f));
            }
            const std::size_t ringIndex = path.size() - 1 - static_cast<std::size_t>(bendSteps / 2);
            // The tail on, far enough for the whole strand.
            float laid = 0.0f;
            for (std::size_t j = 1; j < path.size(); ++j) laid += (path[j] - path[j - 1]).length();
            while (laid < remaining + step && path.size() < kMaxPath) {
                t += tStep;
                const Vec3f next = tailAt(t);
                laid += (next - path.back()).length();
                path.push_back(next);
            }
            // Out of the scalp, all but its start.
            if (surface) {
                RestPoseCollision::Cursor cursor = k == 0 && s < in->roots().size() ? surface.cursor(in->roots()[s], S)
                                                                                     : RestPoseCollision::Cursor{};
                MeshDistanceField::Probe probe;
                probe.beside = 4.0f * clearance;
                for (std::size_t j = 1; j < path.size(); ++j) {
                    probe.forget();
                    surface.pushOut(path[j], clearance, depth, searchRadius, cursor, &probe);
                }
            }
            pathArcs.assign(1, 0.0f);
            for (std::size_t j = 1; j < path.size(); ++j) pathArcs.push_back(pathArcs.back() + (path[j] - path[j - 1]).length());
            ringArc = pathArcs[ringIndex];

            // The strand along it, each segment exactly its length: the first crossing past the
            // last point of the sphere about it of that radius.
            std::size_t seg = 0;  // path segment from path[seg] to path[seg + 1]
            double along = 0.0;   // how far along it the last point is
            Vec3f last = S;
            const std::size_t segments = path.size() - 1;
            for (std::size_t i = k + 1; i < n; ++i) {
                const double L = lengths[i];
                bool found = false;
                for (std::size_t j = seg; j < segments && !found; ++j) {
                    const Vec3f& p0 = path[j];
                    const Vec3f d = path[j + 1] - p0;
                    if ((path[j + 1] - last).length() < L) continue;
                    // |p0 + u d - last| = L, the larger root (leaving the sphere), u in [from, 1].
                    const Vec3f q = p0 - last;
                    const double A = d.dot(d), B = 2.0 * static_cast<double>(d.dot(q)), C = static_cast<double>(q.dot(q)) - L * L;
                    if (A <= 0.0) continue;
                    const double disc = std::max(0.0, B * B - 4.0 * A * C);
                    const double u = std::clamp((-B + std::sqrt(disc)) / (2.0 * A), j == seg ? along : 0.0, 1.0);
                    seg = j;
                    along = u;
                    last = p0 + d * static_cast<float>(u);
                    found = true;
                }
                if (!found) {  // past the path's end: on along its last segment
                    const Vec3f end = path.back();
                    Vec3f dir = (path.back() - path[path.size() - 2]).normalized();
                    if (dir.length() < 0.5f) dir = axis;
                    const Vec3f q = end - last;
                    const double B = 2.0 * static_cast<double>(dir.dot(q)), C = static_cast<double>(q.dot(q)) - L * L;
                    const double u = (-B + std::sqrt(std::max(0.0, B * B - 4.0 * C))) / 2.0;
                    path.push_back(end + dir * static_cast<float>(std::max(u, 0.0)));
                    pathArcs.push_back(pathArcs.back() + static_cast<float>(std::max(u, 0.0)));
                    seg = path.size() - 2;
                    along = 1.0;
                    last = path.back();
                }
                // Exactly its length from the point before, along where it landed.
                const Vec3f d = last - pts[i - 1];
                const float len = d.length();
                pts[i] = len > 0.0f ? pts[i - 1] + d * (lengths[i] / len) : last;
                last = pts[i];
                if (tail) {
                    const float arc = pathArcs[seg] + static_cast<float>(along) * (pathArcs[seg + 1] - pathArcs[seg]);
                    tail[begin + i] = radius > 0.0f ? smoothstep(ringArc - radius, ringArc + radius, arc) : (arc >= ringArc ? 1.0f : 0.0f);
                }
            }
        };
        // The points before each strand's start stay; pts[k] is the start itself.
        parallelFor(0, strands, tie);
        return StrandsPtr(out);
    }
};

}  // namespace

void registerTieOperator(OperatorRegistry& registry) {
    registry.registerType(op::kTie,
                          {"deform",
                           "Ponytails and hair ties: strands run along the scalp to a ring, pass through it in order and fan into a tail.",
                           "ponytail ring hair tie bun"},
                          [] { return std::make_unique<TieOp>(); });
}

}  // namespace groomlab
