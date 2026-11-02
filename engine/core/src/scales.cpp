// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/scales.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/distance_field.h"
#include "groomlab/export_view.h"
#include "groomlab/frames.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265f;

// ---- Built-in plates ----

// Half the plate's width at t along it (0 base, 1 tip): from the base's width up to the widest,
// then down to the tip, pointed (a cosine, straight into the tip) or rounded (a quarter circle).
float halfWidthAt(const ScalePlateShape& shape, float t) {
    const float half = 0.5f * shape.width;
    const float widest = std::clamp(shape.widest, 0.05f, 0.95f);
    if (t <= widest) return half * (shape.base + (1.0f - shape.base) * pm::sin(0.5f * kPi * t / widest));
    const float u = std::min((t - widest) / (1.0f - widest), 1.0f);
    const float pointed = pm::cos(0.5f * kPi * u), rounded = std::sqrt(std::max(0.0f, 1.0f - u * u));
    return std::max(half * (rounded + (pointed - rounded) * std::clamp(shape.tip, 0.0f, 1.0f)), 0.01f * half);
}

}  // namespace

InstancePrototype scalePlate(const ScalePlateShape& shape) {
    // All quads, one closed piece (for subdivision), the top and the underside alike: loops of the
    // same N vertices, each the outline scaled toward a point on the axis (from the rim's middle, a
    // crease loop at the edge, a support loop holding it when subdivided, an inner loop), the inner
    // ones filled with a columns x rows grid of quads (a Coons patch of their four sides). Each loop's
    // sides match the grid's: the base `columns` segments (its middle vertex on the axis), each long
    // side `rows`, the tip end `columns` (its middle vertex the tip).
    const int columns = std::max(2, shape.columns + (shape.columns & 1));  // even: a column of vertices on the axis
    const int rows = std::max(shape.rows, 2);
    const int half = columns / 2;
    const int n = 2 * (columns + rows);
    InstancePrototype out;
    out.found = true;
    const float rimHeight = shape.height * std::clamp(shape.rim, 0.0f, 1.0f);
    const float widthUv = std::max(shape.width, 1e-6f);
    auto add = [&](const Vec3f& p) {
        out.points.push_back(p);
        out.uvs.push_back({0.5f + p.x / widthUv, p.y});
        return static_cast<std::uint32_t>(out.points.size() - 1);
    };
    // Two triangles a quad, one after the other (a, b, c) (a, c, d): ToMesh and the exports read
    // them back as the quad (strand_geometry quadOf).
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        out.triangles.insert(out.triangles.end(), {a, b, c, a, c, d});
    };

    // The outline, counterclockwise seen from +Z from the base's middle (index 0) to the tip
    // (n / 2) and back: the right half's n / 2 segments evenly by arc length along the base to its
    // corner and up the side (even edges: even quads, a smooth subdivision); the left its mirror.
    std::vector<Vec3f> outline(static_cast<std::size_t>(n));
    {
        std::vector<Vec3f> path{{0.0f, 0.0f, 0.0f}};
        constexpr int kDense = 256;
        for (int k = 0; k <= kDense; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(kDense);
            path.push_back({halfWidthAt(shape, t), t, 0.0f});
        }
        path.push_back({0.0f, 1.0f, 0.0f});  // the tip on the axis (a rounded tip ends a hair off it)
        std::vector<float> along(path.size(), 0.0f);
        for (std::size_t k = 1; k < path.size(); ++k) along[k] = along[k - 1] + (path[k] - path[k - 1]).length();
        for (int k = 0; k <= n / 2; ++k) {
            const float target = along.back() * static_cast<float>(k) / static_cast<float>(n / 2);
            const std::size_t i = static_cast<std::size_t>(std::upper_bound(along.begin(), along.end(), target) - along.begin());
            const std::size_t j = std::clamp<std::size_t>(i, 1, path.size() - 1);
            const float span = along[j] - along[j - 1];
            const float f = span > 0.0f ? std::clamp((target - along[j - 1]) / span, 0.0f, 1.0f) : 0.0f;
            outline[static_cast<std::size_t>(k)] = path[j - 1] + (path[j] - path[j - 1]) * f;
        }
        outline[0] = {0.0f, 0.0f, 0.0f};
        outline[static_cast<std::size_t>(n / 2)] = {0.0f, 1.0f, 0.0f};
        for (int k = 1; k < n / 2; ++k) {
            const Vec3f& m = outline[static_cast<std::size_t>(k)];
            outline[static_cast<std::size_t>(n - k)] = {-m.x, m.y, 0.0f};
        }
    }

    // The loops: the outline scaled by `scale` toward (0, kCenter) on the axis, at a height.
    constexpr float kCenter = 0.45f;
    auto loopAt = [&](float scale, const std::function<float(const Vec3f&)>& height) {
        std::vector<std::uint32_t> loop(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            const Vec3f& o = outline[static_cast<std::size_t>(k)];
            Vec3f q{o.x * scale, kCenter + (o.y - kCenter) * scale, 0.0f};
            q.z = height(q);
            loop[static_cast<std::size_t>(k)] = add(q);
        }
        return loop;
    };
    // The top's height: the rim, domed toward the axis and the middle of the length, a keel down it.
    auto top = [&](const Vec3f& q) {
        const float t = std::clamp(q.y, 0.0f, 1.0f);
        const float w = std::max(halfWidthAt(shape, t), 1e-6f);
        const float s = std::clamp(q.x / w, -1.0f, 1.0f);
        const float bump = pm::pow(4.0f * t * (1.0f - t), 0.6f);
        const float keel = shape.keel * std::max(0.0f, 1.0f - 2.5f * std::abs(s)) * std::sqrt(4.0f * t * (1.0f - t));
        return rimHeight + (shape.height - rimHeight) * (1.0f - s * s) * bump + keel;
    };
    auto flat = [](const Vec3f&) { return 0.0f; };
    auto rim = [&](const Vec3f&) { return rimHeight; };
    auto middle = [&](const Vec3f&) { return 0.5f * rimHeight; };
    // The top and the underside the same: from the loop round the rim's middle (the full outline),
    // a crease loop at the rim's edge, a support loop holding it, the inner loop the grid fills.
    const std::vector<std::uint32_t> edge = loopAt(1.0f, middle);
    const std::vector<std::uint32_t> topCrease = loopAt(0.985f, rim);
    const std::vector<std::uint32_t> topSupport = loopAt(0.9f, top);
    const std::vector<std::uint32_t> topInner = loopAt(0.62f, top);
    const std::vector<std::uint32_t> underCrease = loopAt(0.985f, flat);
    const std::vector<std::uint32_t> underSupport = loopAt(0.9f, flat);
    const std::vector<std::uint32_t> underInner = loopAt(0.62f, flat);
    auto band = [&](const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, bool up) {
        // a the loop nearer the rim's middle, b the next one in: the quads face up (the top, the
        // rim's upper half) or down (the underside, its lower half); both loops counterclockwise
        // from +Z.
        for (int k = 0; k < n; ++k) {
            const std::size_t i = static_cast<std::size_t>(k), j = static_cast<std::size_t>((k + 1) % n);
            if (up) quad(a[i], a[j], b[j], b[i]);
            else quad(a[i], b[i], b[j], a[j]);
        }
    };
    band(edge, topCrease, true);
    band(topCrease, topSupport, true);
    band(topSupport, topInner, true);
    band(edge, underCrease, false);
    band(underCrease, underSupport, false);
    band(underSupport, underInner, false);
    // A loop's grid: corner (c, r) for c 0..columns (left to right), r 0..rows (base to tip), its
    // edge the loop's vertices, the inside a Coons patch of them.
    auto fill = [&](const std::vector<std::uint32_t>& loop, bool up, const std::function<float(const Vec3f&)>& height) {
        auto edgeAt = [&](int c, int r) -> int {  // the loop index of an edge corner, or -1 inside
            if (r == 0) return (c - half + n) % n;
            if (c == columns) return half + r;
            if (r == rows) return half + rows + (columns - c);
            if (c == 0) return (n - half - r + n) % n;
            return -1;
        };
        std::vector<std::uint32_t> grid(static_cast<std::size_t>((columns + 1) * (rows + 1)));
        auto at = [&](int c, int r) -> std::uint32_t& { return grid[static_cast<std::size_t>(r * (columns + 1) + c)]; };
        auto pos = [&](int c, int r) { return out.points[loop[static_cast<std::size_t>(edgeAt(c, r))]]; };
        for (int r = 0; r <= rows; ++r) {
            for (int c = 0; c <= columns; ++c) {
                const int e = edgeAt(c, r);
                if (e >= 0) {
                    at(c, r) = loop[static_cast<std::size_t>(e)];
                    continue;
                }
                const float u = static_cast<float>(c) / static_cast<float>(columns), v = static_cast<float>(r) / static_cast<float>(rows);
                const Vec3f q = pos(c, 0) * (1.0f - v) + pos(c, rows) * v + pos(0, r) * (1.0f - u) + pos(columns, r) * u -
                                (pos(0, 0) * ((1.0f - u) * (1.0f - v)) + pos(columns, 0) * (u * (1.0f - v)) +
                                 pos(0, rows) * ((1.0f - u) * v) + pos(columns, rows) * (u * v));
                at(c, r) = add({q.x, q.y, height(q)});
            }
        }
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < columns; ++c) {
                if (up) quad(at(c, r), at(c + 1, r), at(c + 1, r + 1), at(c, r + 1));
                else quad(at(c, r), at(c, r + 1), at(c + 1, r + 1), at(c + 1, r));
            }
        }
    };
    fill(topInner, true, top);
    fill(underInner, false, flat);

    // Smooth normals over the whole closed piece (the crease and support loops keep the edges).
    out.normals.assign(out.points.size(), Vec3f{});
    for (std::size_t k = 0; k + 2 < out.triangles.size(); k += 3) {
        const Vec3f& a = out.points[out.triangles[k]];
        const Vec3f face = (out.points[out.triangles[k + 1]] - a).cross(out.points[out.triangles[k + 2]] - a);
        for (int c = 0; c < 3; ++c) out.normals[out.triangles[k + static_cast<std::size_t>(c)]] += face;
    }
    for (Vec3f& v : out.normals) v = v.length() > 1e-20f ? v.normalized() : Vec3f{0, 0, 1};
    std::uint64_t signature = hash64(0x5CA1E);
    for (const Vec3f& p : out.points) signature = hashFloat(hashFloat(hashFloat(signature, p.x), p.y), p.z);
    out.signature = signature ? signature : 1;
    return out;
}

namespace {

struct Builtin {
    const char* name;
    ScalePlateShape shape;
};

ScalePlateShape roundShape() {
    ScalePlateShape s;
    s.base = 0.6f;
    s.widest = 0.45f;
    s.tip = 0.0f;
    s.keel = 0.0f;
    return s;
}

ScalePlateShape shieldShape() {
    ScalePlateShape s;
    s.base = 0.85f;
    s.widest = 0.5f;
    s.tip = 0.3f;
    s.height = 0.07f;
    s.rim = 0.5f;
    s.keel = 0.0f;
    return s;
}

const std::map<std::string, InstancePrototype>& builtins() {
    static const std::map<std::string, InstancePrototype> made = [] {
        std::map<std::string, InstancePrototype> m;
        m["groomlab:scale"] = scalePlate(ScalePlateShape{});
        m["groomlab:scaleRound"] = scalePlate(roundShape());
        m["groomlab:scaleShield"] = scalePlate(shieldShape());
        return m;
    }();
    return made;
}

}  // namespace

const InstancePrototype* builtinPrototype(const std::string& name) {
    if (name.rfind("groomlab:", 0) != 0) return nullptr;
    const auto& all = builtins();
    auto it = all.find(name);
    return it == all.end() ? nullptr : &it->second;
}

std::vector<std::string> builtinPrototypeNames() { return {"groomlab:scale", "groomlab:scaleRound", "groomlab:scaleShield"}; }

std::vector<std::string> graphMeshPrototypes(const std::string& graphJson) {
    std::set<std::string> names;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(graphJson.empty() ? std::string("{}") : graphJson);
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    // Every object of type Instance, however deep (a compound keeps its graph inside its node).
    std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& j) {
        if (j.is_object()) {
            const auto type = j.find("type");
            const auto params = j.find("params");
            if (type != j.end() && type->is_string() && (type->get<std::string>() == "Instance" || type->get<std::string>() == "Scales") && params != j.end() &&
                params->is_object()) {
                const auto prototype = params->find("prototype");
                if (prototype != params->end() && prototype->is_string()) {
                    for (const std::string& name : instancePrototypeNames(prototype->get<std::string>())) {
                        if (!name.empty() && !builtinPrototype(name)) names.insert(name);
                    }
                }
            }
            for (const auto& item : j) walk(item);
        } else if (j.is_array()) {
            for (const auto& item : j) walk(item);
        }
    };
    walk(root);
    return {names.begin(), names.end()};
}

std::vector<std::string> missingInstancePrototypes(const std::string& prototypeParam,
                                                   const std::function<bool(const std::string&)>& has) {
    std::vector<std::string> missing;
    for (const std::string& name : instancePrototypeNames(prototypeParam)) {
        if (name.empty() || builtinPrototype(name) || (has && has(name))) continue;
        if (std::find(missing.begin(), missing.end(), name) == missing.end()) missing.push_back(name);
    }
    return missing;
}

void ScaleMesh::Scale::matrix(float out[16]) const {
    InstanceSet::Placed p;
    p.x = x;
    p.y = y;
    p.z = z;
    p.origin = origin;
    p.matrix(out);
}

// ---- The scales' mesh ----

namespace {

// What the bend needs of a prototype, worked out once per build.
struct Proto {
    const InstancePrototype* mesh = nullptr;
    float minY = 0.0f, length = 1.0f, height = 0.1f;
    std::uint64_t signature = 0;
};

// A prototype's point (or normal) in the roles a deformed instance gives its axes: the long axis
// (deformAxis) along the strand as +Y, the next two cyclically as +Z and +X (a turn, not a mirror:
// axis y keeps x, y, z as they are).
Vec3f deformRoles(const Vec3f& v, std::uint8_t axis) {
    switch (axis) {
        case 0: return {v.z, v.x, v.y};  // x along: y up, z across
        case 2: return {v.y, v.z, v.x};  // z along: x up, y across
        default: return v;
    }
}

Proto protoOf(const InstancePrototype* mesh) {
    Proto p;
    p.mesh = mesh;
    if (!mesh || mesh->points.empty() || mesh->triangles.empty()) {
        p.mesh = nullptr;
        return p;
    }
    float lo = mesh->points[0].y, hi = lo, top = 0.0f;
    for (const Vec3f& q : mesh->points) {
        lo = std::min(lo, q.y);
        hi = std::max(hi, q.y);
        top = std::max(top, q.z);
    }
    p.minY = lo;
    p.length = hi > lo ? hi - lo : 1.0f;
    p.height = top > 0.0f ? top : 0.1f * p.length;  // a flat prototype tiles as a thin plate would
    // The host's fingerprint (its points, or points and triangles) and the normals the bend turns.
    std::uint64_t signature = hash64(mesh->signature);
    for (const Vec3f& n : mesh->normals) signature = hashFloat(hashFloat(hashFloat(signature, n.x), n.y), n.z);
    p.signature = signature;
    return p;
}

// The rotation taking unit vector a onto unit vector b, applied to v.
Vec3f rotateOnto(const Vec3f& a, const Vec3f& b, const Vec3f& v) {
    const float c = a.dot(b);
    if (c > 1.0f - 1e-7f) return v;
    if (c < -1.0f + 1e-6f) return v * -1.0f;  // (opposite: never on a skin a scale lies on)
    const Vec3f k = a.cross(b);
    return v * c + k.cross(v) + k * (k.dot(v) / (1.0f + c));
}

// The share of a plate's length (from its base) over which a scale following its strand leaves the
// skin: its base flat on the skin, past this all along the strand.
constexpr float kStrandBase = 0.25f;

// A strand as a scale following it reads it (follow strand): positions and frames by the distance
// from the root, the width axis carried along from the scale's (parallel transport: no twist).
struct StrandSpine {
    std::vector<Vec3f> points, tangents, across;
    std::vector<float> distances;  // from the root, at each point

    // `across`: the scale's +X at the root.
    StrandSpine(const Vec3f* pts, std::size_t n, const Vec3f& acrossAtRoot) {
        points.assign(pts, pts + n);
        distances.resize(n, 0.0f);
        tangents.resize(n);
        across.resize(n);
        for (std::size_t i = 1; i < n; ++i) distances[i] = distances[i - 1] + (pts[i] - pts[i - 1]).length();
        // Each point's tangent: its segments' directions together (the ends: their one segment).
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3f in = i > 0 ? pts[i] - pts[i - 1] : Vec3f{};
            const Vec3f out = i + 1 < n ? pts[i + 1] - pts[i] : Vec3f{};
            const Vec3f t = (in.length() > 0.0f ? in.normalized() : in) + (out.length() > 0.0f ? out.normalized() : out);
            tangents[i] = t.length() > 1e-12f ? t.normalized() : (i > 0 ? tangents[i - 1] : Vec3f{0, 1, 0});
        }
        Vec3f x = acrossAtRoot - tangents[0] * acrossAtRoot.dot(tangents[0]);
        x = x.length() > 1e-12f ? x.normalized() : anyPerpendicular(tangents[0]).normalized();
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) {
                x = rotateOnto(tangents[i - 1], tangents[i], x);
                x = x - tangents[i] * x.dot(tangents[i]);
                x = x.length() > 1e-12f ? x.normalized() : anyPerpendicular(tangents[i]).normalized();
            }
            across[i] = x;
        }
    }
    // At `d` from the root: the point, the tangent and the width axis (past the tip, straight on;
    // before the root, with `back`, straight back, else at the root).
    void at(float d, Vec3f* p, Vec3f* t, Vec3f* x, bool back = false) const {
        const std::size_t n = points.size();
        if (back && d < 0.0f) {
            *t = tangents[0];
            *x = across[0];
            *p = points[0] + *t * d;
            return;
        }
        if (d >= distances[n - 1]) {
            *t = tangents[n - 1];
            *x = across[n - 1];
            *p = points[n - 1] + *t * (d - distances[n - 1]);
            return;
        }
        const std::size_t i = static_cast<std::size_t>(std::upper_bound(distances.begin(), distances.end(), std::max(d, 0.0f)) -
                                                       distances.begin());
        const std::size_t a = std::clamp<std::size_t>(i, 1, n - 1) - 1, b = a + 1;
        const float span = distances[b] - distances[a];
        const float u = span > 0.0f ? std::clamp((std::max(d, 0.0f) - distances[a]) / span, 0.0f, 1.0f) : 0.0f;
        *p = points[a] + (points[b] - points[a]) * u;
        const Vec3f tt = tangents[a] * (1.0f - u) + tangents[b] * u;
        *t = tt.length() > 1e-12f ? tt.normalized() : tangents[a];
        Vec3f xx = across[a] * (1.0f - u) + across[b] * u;
        xx = xx - *t * xx.dot(*t);
        *x = xx.length() > 1e-12f ? xx.normalized() : across[a];
    }
};

// A strand's color for its scale: HairColor's RGB per point (at `t` along it) or per strand, else
// its melanin's, else the default hair color.
struct StrandColorReader {
    std::array<const std::vector<float>*, 3> points{}, strands{};
    const std::vector<float>* melanin = nullptr;
    const std::vector<float>* redness = nullptr;
    const StrandSet* set = nullptr;

    explicit StrandColorReader(const StrandSet& s) : set(&s) {
        bool perPoint = true, perStrand = true;
        for (int k = 0; k < 3; ++k) {
            points[static_cast<std::size_t>(k)] = s.pointChannel(kColorChannels[k]);
            strands[static_cast<std::size_t>(k)] = s.strandChannel(kColorChannels[k]);
            perPoint = perPoint && points[static_cast<std::size_t>(k)] && points[static_cast<std::size_t>(k)]->size() == s.pointCount();
            perStrand = perStrand && strands[static_cast<std::size_t>(k)] &&
                        strands[static_cast<std::size_t>(k)]->size() == s.strandCount();
        }
        if (!perPoint) points = {};
        if (!perStrand) strands = {};
        melanin = s.strandChannel(kMelaninChannel);
        if (melanin && melanin->size() != s.strandCount()) melanin = nullptr;
        redness = s.strandChannel(kMelaninRednessChannel);
        if (redness && redness->size() != s.strandCount()) redness = nullptr;
    }
    std::array<float, 3> at(std::size_t s, float t) const {
        if (points[0]) {
            const std::size_t n = set->strandPointCount(s), begin = set->strandBegin(s);
            if (n > 0) {
                const float f = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
                const auto i = std::min(static_cast<std::size_t>(f), n - 1);
                const std::size_t j = std::min(i + 1, n - 1);
                const float u = f - static_cast<float>(i);
                std::array<float, 3> c;
                for (std::size_t k = 0; k < 3; ++k) c[k] = (*points[k])[begin + i] * (1.0f - u) + (*points[k])[begin + j] * u;
                return c;
            }
        }
        if (strands[0]) return {(*strands[0])[s], (*strands[1])[s], (*strands[2])[s]};
        if (melanin) return melaninColor((*melanin)[s], redness ? (*redness)[s] : kDefaultMelaninRedness);
        return defaultHairColor();
    }
};

}  // namespace

void ScaleMeshBuilder::clear() {
    last_.reset();
    kept_.clear();
    stats_ = {};
}

std::shared_ptr<const ScaleMesh> ScaleMeshBuilder::build(const StrandSet& strands, const std::shared_ptr<const ScalpMesh>& scalp,
                                                         const PrototypeLookup& prototypes, const ScaleMeshOptions& options) {
    stats_ = {};
    const InstanceSet* set = strands.instances().get();
    if (!set || !set->hasMeshes()) {
        clear();
        return nullptr;
    }
    const ScalpMesh* skin = scalp && scalp->triangleCount() > 0 ? scalp.get() : nullptr;
    std::vector<InstanceSet::Placed> placed = set->resolve(strands, skin);
    placed.erase(std::remove_if(placed.begin(), placed.end(), [](const InstanceSet::Placed& p) { return !p.meshed; }), placed.end());
    if (placed.empty()) {
        clear();
        return nullptr;
    }
    // Each prototype once: the built-in plates, else the host's.
    std::vector<Proto> protos(set->prototypes.size());
    for (std::size_t p = 0; p < protos.size(); ++p) {
        const InstancePrototype* mesh = builtinPrototype(set->prototypes[p]);
        if (!mesh && prototypes) {
            mesh = prototypes(set->prototypes[p]);
            if (mesh && !mesh->found) mesh = nullptr;
        }
        protos[p] = protoOf(mesh);
    }
    auto protoFor = [&](const InstanceSet::Placed& p) -> const Proto& {
        static const Proto kNone;
        return p.prototype < protos.size() ? protos[p.prototype] : kNone;
    };
    // Meshes or quads: a render takes meshes; the viewport quads for preview quads, and for auto
    // when the meshes would pass the budget.
    const std::size_t count = placed.size();
    std::vector<char> quad(count, 0);
    std::size_t autoTriangles = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const InstanceSet::Instance& inst = set->instances[placed[k].index];
        const Proto& proto = protoFor(placed[k]);
        if (!proto.mesh || (options.preview && inst.preview == 2)) {
            quad[k] = 1;
        } else if (options.preview && inst.preview == 0) {
            autoTriangles += proto.mesh->triangles.size() / 3;
        }
    }
    if (options.preview && options.triangleBudget > 0 && autoTriangles > options.triangleBudget) {
        for (std::size_t k = 0; k < count; ++k) {
            if (set->instances[placed[k].index].preview == 0) quad[k] = 1;
        }
    }
    // Where each scale's vertices and triangles go.
    auto mesh = std::make_shared<ScaleMesh>();
    mesh->scales.resize(count);
    std::size_t vertices = 0, triangles = 0;
    for (std::size_t k = 0; k < count; ++k) {
        ScaleMesh::Scale& sc = mesh->scales[k];
        const Proto& proto = protoFor(placed[k]);
        sc.firstVertex = static_cast<std::uint32_t>(vertices);
        sc.firstTriangle = static_cast<std::uint32_t>(triangles);
        sc.vertexCount = quad[k] ? 4u : static_cast<std::uint32_t>(proto.mesh->points.size());
        sc.triangleCount = quad[k] ? 2u : static_cast<std::uint32_t>(proto.mesh->triangles.size() / 3);
        sc.quad = quad[k] != 0;
        vertices += sc.vertexCount;
        triangles += sc.triangleCount;
    }
    mesh->points.resize(vertices);
    mesh->normals.resize(vertices);
    mesh->tangents.resize(vertices);
    mesh->uvs.resize(vertices);
    mesh->colors.resize(vertices);
    mesh->triangles.resize(triangles * 3);
    // Strands by ID (colors, root uvs) and the skin's field for the bend, made for the largest scale.
    std::unordered_map<std::uint64_t, std::size_t> strandIndex;
    strandIndex.reserve(strands.strandCount());
    for (std::size_t s = 0; s < strands.strandCount(); ++s) strandIndex.emplace(strands.ids()[s], s);
    float reach = 0.0f, lowest = 0.0f;
    bool bends = false;
    for (std::size_t k = 0; k < count; ++k) {
        const InstanceSet::Instance& inst = set->instances[placed[k].index];
        if (quad[k] || !inst.onSkin || !inst.conform) continue;
        reach = std::max({reach, placed[k].y.length(), placed[k].x.length()});
        lowest = bends ? std::min(lowest, inst.offset) : inst.offset;
        bends = true;
    }
    const DistanceFieldPtr field = bends && skin ? collisionField(scalp, std::max(lowest, 0.0f), 2.0f * reach) : nullptr;
    // The skin and the field the bend samples (its offset and reach are every scale's: another
    // scale's change makes another field, and a kept scale bent on the last one is not this one's).
    const std::uint64_t skinKey = skin ? hashFloat(hashFloat(skin->surfaceHash(), std::max(lowest, 0.0f)), 2.0f * reach) : 0;
    const StrandColorReader colors(strands);
    const ScaleMesh* last = last_.get();
    std::vector<std::uint64_t> keys(count, 0);
    std::vector<char> made(count, 0);  // 1 kept from the last build, 2 bent now

    parallelFor(0, count, [&](std::size_t k) {
        const InstanceSet::Placed& p = placed[k];
        const InstanceSet::Instance& inst = set->instances[p.index];
        const Proto& proto = protoFor(p);
        ScaleMesh::Scale& sc = mesh->scales[k];
        sc.id = p.id;
        sc.strand = inst.strand;
        sc.index = p.index;
        sc.prototype = p.prototype;
        sc.variant = inst.variant;
        sc.material = inst.material;
        sc.random = unitFloat(hash64(p.id ^ 0x5CA1E5EEDull));
        const auto found = strandIndex.find(inst.strand);
        const std::size_t s = found != strandIndex.end() ? found->second : 0;
        const StrandRoot root = found != strandIndex.end() ? strands.roots()[s] : StrandRoot{};
        if (skin && skin->contains(root)) sc.rootUv = skin->uvAt(root);
        // Bent along its strand (not a scale): its frame where it sits, no tilt, raise or bend
        // onto the skin; its vertices laid along the strand below.
        const bool deformed = !inst.onSkin && inst.deform;
        const float strandLength =
            found != strandIndex.end() && strands.strandPointCount(s) > 1 ? arcLength(strands.strandPoints(s), strands.strandPointCount(s)) : 0.0f;
        sc.color = colors.at(s, deformed && strandLength > 0.0f ? p.distance / strandLength : 0.0f);
        // The frame: the tip turned up about +X at the base by the lift, and by as much again as
        // raises it `tile` of the plate's height (onto the next scale's base); raised off the skin
        // by the offset and a little of its own (stack: its own height, so overlapping scales
        // never share a depth).
        const float yLength = p.y.length(), zLength = p.z.length();
        const Vec3f normal = zLength > 0.0f ? p.z / zLength : Vec3f{0, 0, 1};
        const Vec3f along = yLength > 0.0f ? p.y / yLength : Vec3f{0, 1, 0};
        const float plateLength = proto.length * yLength, plateHeight = proto.height * zLength;
        const float tileAngle = plateLength > 0.0f ? pm::atan2(std::max(inst.tile, 0.0f) * plateHeight, plateLength) : 0.0f;
        const float angle = deformed ? 0.0f : inst.lift + tileAngle;
        const float ca = pm::cos(angle), sa = pm::sin(angle);
        const Vec3f yDir = along * ca + normal * sa, zDir = normal * ca - along * sa;
        // (A deformed instance's offset is in its origin already: off the strand along +X.)
        const float raise =
            deformed ? 0.0f : inst.offset + std::max(inst.stack, 0.0f) * plateHeight * unitFloat(hash64(p.id ^ 0x57AC4ull));
        sc.x = p.x;
        sc.y = yDir * yLength;
        sc.z = zDir * zLength;
        sc.origin = p.origin + normal * raise;
        const bool bend = !sc.quad && inst.onSkin && inst.conform && skin;
        const bool follow = !sc.quad && inst.onSkin && inst.followStrand && found != strandIndex.end() && strands.strandPointCount(s) > 1;
        const bool alongStrand = !sc.quad && deformed && found != strandIndex.end() && strands.strandPointCount(s) > 1;
        // What the bent vertices come from: kept from the last build when it is all the same (and,
        // following the strand, the strand the same).
        std::uint64_t key = hashCombine(hashCombine(hash64(proto.signature), skinKey),
                                        (sc.quad ? 1u : 0u) | (bend ? 2u : 0u) | (follow ? 4u : 0u) |
                                            (alongStrand ? 8u + 16u * inst.deformAxis : 0u));
        for (const Vec3f& v : {sc.x, sc.y, sc.z, sc.origin, normal}) key = hashFloat(hashFloat(hashFloat(key, v.x), v.y), v.z);
        key = hashFloat(key, inst.offset);
        if (alongStrand) key = hashFloat(key, p.distance);
        if (follow || alongStrand) {
            const Vec3f* pts = strands.strandPoints(s);
            for (std::size_t i = 0; i < strands.strandPointCount(s); ++i) {
                key = hashFloat(hashFloat(hashFloat(key, pts[i].x), pts[i].y), pts[i].z);
            }
        }
        keys[k] = key;
        const std::size_t v0 = sc.firstVertex;
        // Triangles and uvs (their prototype's, offset to the scale's first vertex).
        if (sc.quad) {
            const std::uint32_t b = sc.firstVertex;
            const std::uint32_t quadTriangles[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
            std::copy(quadTriangles, quadTriangles + 6, mesh->triangles.begin() + static_cast<std::ptrdiff_t>(sc.firstTriangle) * 3);
            const std::array<float, 2> quadUvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (std::size_t v = 0; v < 4; ++v) mesh->uvs[v0 + v] = quadUvs[v];
        } else {
            const std::vector<std::uint32_t>& tris = proto.mesh->triangles;
            for (std::size_t t = 0; t < tris.size(); ++t) mesh->triangles[sc.firstTriangle * 3 + t] = sc.firstVertex + tris[t];
            const bool uvs = proto.mesh->uvs.size() == proto.mesh->points.size();
            for (std::size_t v = 0; v < sc.vertexCount; ++v) mesh->uvs[v0 + v] = uvs ? proto.mesh->uvs[v] : std::array<float, 2>{0, 0};
        }
        // Colors along the strand, base to tip (bent along it: where each vertex lies on it).
        for (std::size_t v = 0; v < sc.vertexCount; ++v) {
            float t = sc.quad ? (v < 2 ? 0.0f : 1.0f) : (proto.mesh->points[v].y - proto.minY) / proto.length;
            if (deformed && strandLength > 0.0f) {
                const float along = sc.quad ? (v < 2 ? 0.0f : 1.0f) : deformRoles(proto.mesh->points[v], inst.deformAxis).y;
                t = (p.distance + along * yLength) / strandLength;
            }
            mesh->colors[v0 + v] = colors.at(s, t);
        }
        if (last) {
            auto it = kept_.find(p.id);
            if (it != kept_.end() && it->second.key == key && it->second.vertexCount == sc.vertexCount) {
                const std::size_t from = it->second.firstVertex;
                std::copy_n(last->points.begin() + static_cast<std::ptrdiff_t>(from), sc.vertexCount, mesh->points.begin() + static_cast<std::ptrdiff_t>(v0));
                std::copy_n(last->normals.begin() + static_cast<std::ptrdiff_t>(from), sc.vertexCount, mesh->normals.begin() + static_cast<std::ptrdiff_t>(v0));
                std::copy_n(last->tangents.begin() + static_cast<std::ptrdiff_t>(from), sc.vertexCount, mesh->tangents.begin() + static_cast<std::ptrdiff_t>(v0));
                made[k] = 1;
                return;
            }
        }
        if (sc.quad) {
            const Vec3f corners[4] = {sc.x * -0.5f, sc.x * 0.5f, sc.x * 0.5f + sc.y, sc.x * -0.5f + sc.y};
            for (std::size_t v = 0; v < 4; ++v) {
                mesh->points[v0 + v] = sc.origin + corners[v];
                mesh->normals[v0 + v] = zDir;
                mesh->tangents[v0 + v] = yDir;
            }
            return;
        }
        // Normals through the inverse transpose of the (right-angled, scaled) frame.
        auto inverseSquared = [](const Vec3f& a) {
            const float l2 = a.dot(a);
            return l2 > 1e-30f ? a / l2 : a;
        };
        const Vec3f nx = inverseSquared(sc.x), ny = inverseSquared(sc.y), nz = inverseSquared(sc.z);
        const float searchRadius = 2.0f * std::max(plateLength, sc.x.length()) + raise;
        if (bend || follow || alongStrand) made[k] = 2;
        // Following the strand: its curve over the plate's length, the width axis carried along.
        std::unique_ptr<StrandSpine> spine;
        if (follow) spine = std::make_unique<StrandSpine>(strands.strandPoints(s), strands.strandPointCount(s), p.x);
        const float xLength = std::max(sc.x.length(), 1e-12f), yScale = std::max(yLength, 1e-12f),
                    zScale = std::max(zLength, 1e-12f);
        if (alongStrand) {
            // Each vertex at its place along the prototype's long axis laid onto the strand from
            // where the instance sits, across it in the strand's frame there: the width axis
            // carried along the strand (no twist), turned to the instance's +X where it sits.
            const StrandSpine spine(strands.strandPoints(s), strands.strandPointCount(s), p.x);
            Vec3f c0, t0, x0;
            spine.at(p.distance, &c0, &t0, &x0, true);
            const Vec3f want = (p.x - t0 * p.x.dot(t0)).length() > 1e-12f ? (p.x - t0 * p.x.dot(t0)).normalized() : x0;
            const float twist = pm::atan2(x0.cross(want).dot(t0), x0.dot(want));
            const float offsetAcross = inst.offset;
            for (std::size_t v = 0; v < sc.vertexCount; ++v) {
                const Vec3f q = deformRoles(proto.mesh->points[v], inst.deformAxis);
                const Vec3f m = deformRoles(proto.mesh->normals[v], inst.deformAxis);
                Vec3f c, t, bx;
                spine.at(p.distance + q.y * yLength, &c, &t, &bx, true);
                bx = rotateAxisAngle(bx, t, twist);
                const Vec3f up = bx.cross(t);
                mesh->points[v0 + v] = c + bx * (q.x * xLength + offsetAcross) + up * (q.z * zLength);
                Vec3f n = bx * (m.x / xLength) + t * (m.y / yScale) + up * (m.z / zScale);
                mesh->normals[v0 + v] = n.length() > 1e-12f ? n.normalized() : up;
                mesh->tangents[v0 + v] = t;
            }
            return;
        }
        for (std::size_t v = 0; v < sc.vertexCount; ++v) {
            const Vec3f& q = proto.mesh->points[v];
            const Vec3f& m = proto.mesh->normals[v];
            Vec3f w = sc.origin + sc.x * q.x + sc.y * q.y + sc.z * q.z;
            Vec3f n = nx * m.x + ny * m.y + nz * m.z;
            n = n.length() > 1e-12f ? n.normalized() : zDir;
            Vec3f tangent = yDir;
            if (bend) {
                // Its height above the root's tangent plane (the skin under the base), kept above
                // the skin under it: the point on the plane laid onto the surface along the normal,
                // raised as high again, never below the offset.
                const Vec3f base = p.origin;
                const float h = (w - base).dot(normal);
                const Vec3f onPlane = w - normal * h;
                float height = 0.0f;
                Vec3f surfaceNormal;
                Vec3f surface;
                bool hit = false;
                if (field && field->sample(onPlane, &height, &surfaceNormal)) {
                    surface = onPlane - surfaceNormal * height;
                    hit = true;
                } else {
                    const SurfacePoint sp = skin->closestPoint(onPlane, searchRadius);
                    if (sp.root.face >= 0) {
                        surface = sp.position;
                        surfaceNormal = sp.normal;
                        hit = true;
                    }
                }
                if (hit && surfaceNormal.length() > 1e-6f) {
                    surfaceNormal = surfaceNormal.normalized();
                    // (h counts the raise already: the offset and stack above the root.)
                    w = surface + surfaceNormal * std::max(h, inst.offset);
                    n = rotateOnto(normal, surfaceNormal, n);
                    tangent = rotateOnto(normal, surfaceNormal, tangent);
                }
            }
            if (follow) {
                // The same vertex about the strand's curve where it is along the plate (tangent for
                // +Y, the carried width axis for +X, their normal for +Z, raised as on the skin),
                // blended in from the base: flat on the skin there, all the strand's past kStrandBase.
                Vec3f c, t, bx;
                spine->at(q.y * yLength, &c, &t, &bx);
                const Vec3f up = bx.cross(t);
                const Vec3f onStrand = c + bx * (q.x * xLength) + up * (q.z * zLength + raise);
                Vec3f strandNormal = bx * (m.x / xLength) + t * (m.y / yScale) + up * (m.z / zScale);
                strandNormal = strandNormal.length() > 1e-12f ? strandNormal.normalized() : up;
                const float f = std::clamp((q.y - proto.minY) / (proto.length * kStrandBase), 0.0f, 1.0f);
                const float b = f * f * (3.0f - 2.0f * f);
                w = w * (1.0f - b) + onStrand * b;
                const Vec3f nb = n * (1.0f - b) + strandNormal * b, tb = tangent * (1.0f - b) + t * b;
                n = nb.length() > 1e-12f ? nb.normalized() : strandNormal;
                tangent = tb.length() > 1e-12f ? tb.normalized() : t;
            }
            if (bend) {
                // Never inside: a point the bend left under the offset (a fold, a crease) is pushed out.
                pushOutOf(*skin, field.get(), w, std::max(inst.offset, 0.0f), std::max(plateLength, 1e-6f), searchRadius);
            }
            mesh->points[v0 + v] = w;
            mesh->normals[v0 + v] = n;
            mesh->tangents[v0 + v] = tangent;
        }
    }, 16);

    // Keep each scale's vertices for the next build.
    kept_.clear();
    kept_.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        const ScaleMesh::Scale& sc = mesh->scales[k];
        kept_[sc.id] = Kept{keys[k], sc.firstVertex, sc.vertexCount};
        stats_.quads += sc.quad ? 1 : 0;
        stats_.kept += made[k] == 1 ? 1 : 0;
        stats_.bent += made[k] == 2 ? 1 : 0;
    }
    stats_.scales = count;
    last_ = mesh;
    return mesh;
}

std::shared_ptr<const ScaleMesh> buildScaleMesh(const StrandSet& strands, const std::shared_ptr<const ScalpMesh>& scalp,
                                                const PrototypeLookup& prototypes, const ScaleMeshOptions& options) {
    ScaleMeshBuilder builder;
    return builder.build(strands, scalp, prototypes, options);
}

}  // namespace groomlab
