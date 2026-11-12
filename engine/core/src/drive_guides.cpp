// SPDX-License-Identifier: Apache-2.0
// Driver curves deforming guides (drive_guides.h).

#include "groomlab/drive_guides.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "groomlab/follow_scalp.h"
#include "groomlab/frames.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parallel.h"
#include "groomlab/ramp.h"
#include "groomlab/random.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxDrivers = 8;

// The frames a bound driver's offsets are kept in: rotation-minimizing along it, the first normal
// any perpendicular of its first tangent.
void boundFrames(const std::vector<Vec3f>& points, std::vector<Frame>& out) {
    const Vec3f first = points.size() > 1 ? points[1] - points[0] : Vec3f{0, 0, 1};
    rotationMinimizingFramesInto(points.data(), points.size(), anyPerpendicular(first.normalized()), out);
}

// The frames of a driver now: rotation-minimizing along it, its first normal the bound one turned
// by the least rotation taking the bound first tangent to the current one (so a driver as bound
// has its bound frames, and one swung about its root carries the offsets round with it).
void currentFrames(const std::vector<Vec3f>& points, const std::vector<Frame>& bound, std::vector<Frame>& out) {
    rotationMinimizingFramesInto(points.data(), points.size(), bound[0].normal, out);
    const Rotation turn(bound[0].tangent, out[0].tangent);
    rotationMinimizingFramesInto(points.data(), points.size(), turn(bound[0].normal), out);
}

struct Closest {
    std::uint32_t segment = 0;
    float t = 0.0f;
    float distance = 0.0f;
};

Closest closestOn(const std::vector<Vec3f>& line, const Vec3f& p) {
    Closest best;
    float bestSq = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        const Vec3f ab = line[k + 1] - line[k];
        const float len2 = ab.dot(ab);
        const float t = len2 > 0.0f ? std::clamp((p - line[k]).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
        const Vec3f d = p - (line[k] + ab * t);
        const float dd = d.dot(d);
        if (dd < bestSq) {
            bestSq = dd;
            best.segment = static_cast<std::uint32_t>(k);
            best.t = t;
        }
    }
    best.distance = std::sqrt(bestSq);
    return best;
}

Vec3f along(const std::vector<Vec3f>& line, std::uint32_t segment, float t) {
    return line[segment] + (line[segment + 1] - line[segment]) * t;
}

// The bound drivers a binding can use: 2 points or more.
std::vector<std::uint32_t> usableDrivers(const GuideSet& bound) {
    std::vector<std::uint32_t> out;
    for (std::size_t d = 0; d < bound.guides().size(); ++d) {
        if (bound.guides()[d].points.size() >= 2) out.push_back(static_cast<std::uint32_t>(d));
    }
    return out;
}

GuidesPtr carried(const GuidesPtr& drivers, const ScalpMesh* from, const ScalpMesh* to,
                  const std::unordered_map<std::uint64_t, StrandRoot>* anchors) {
    if (!drivers || !from || !to || from == to || from->triangleCount() == 0 || to->triangleCount() == 0) return drivers;
    std::shared_ptr<GuideSet> out;
    for (std::size_t g = 0; g < drivers->guides().size(); ++g) {
        const Guide& driver = drivers->guides()[g];
        StrandRoot anchor = driver.root;
        if (anchors) {
            const auto it = anchors->find(driver.id);
            if (it == anchors->end()) continue;
            anchor = it->second;
        }
        if (anchor.face < 0) continue;
        const RootMotion motion = rootMotion(*from, *to, anchor);
        if (motion.still()) continue;
        if (!out) out = std::make_shared<GuideSet>(*drivers);
        for (Vec3f& p : out->guides()[g].points) p = motion.point(p);
    }
    return out ? GuidesPtr(out) : drivers;
}

}  // namespace

std::string DriverBind::toJson() const {
    json list = json::array();
    for (const Guide& d : drivers.guides()) {
        json points = json::array();
        for (const Vec3f& p : d.points) {
            points.push_back(p.x);
            points.push_back(p.y);
            points.push_back(p.z);
        }
        list.push_back({{"id", d.id}, {"face", d.root.face}, {"u", d.root.u}, {"v", d.root.v}, {"points", std::move(points)}});
    }
    json out{{"frame", frame}, {"drivers", std::move(list)}};
    if (!attached.empty()) {
        json guides = json::object();
        for (const auto& [guide, ids] : attached) guides[std::to_string(guide)] = ids;
        out["attached"] = std::move(guides);
    }
    return out.dump();
}

DriverBind DriverBind::fromJson(const std::string& text) {
    DriverBind bind;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return bind;
    try {
        const json root = json::parse(text);
        if (!root.is_object()) throw std::invalid_argument("not an object");
        bind.frame = root.value("frame", 0);
        for (const json& d : root.value("drivers", json::array())) {
            const json& flat = d.at("points");
            std::vector<Vec3f> points;
            for (std::size_t i = 0; i + 2 < flat.size(); i += 3) {
                points.push_back({flat[i].get<float>(), flat[i + 1].get<float>(), flat[i + 2].get<float>()});
            }
            Guide& g = bind.drivers.add(d.at("id").get<std::uint64_t>(), std::move(points));
            g.root.face = d.value("face", -1);
            g.root.u = d.value("u", 0.0f);
            g.root.v = d.value("v", 0.0f);
        }
        const json attached = root.value("attached", json::object());
        for (auto it = attached.begin(); it != attached.end(); ++it) {
            std::vector<std::uint64_t> ids = it.value().get<std::vector<std::uint64_t>>();
            if (!ids.empty()) bind.attached[std::stoull(it.key())] = std::move(ids);
        }
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("driverBind: ") + e.what());
    }
    return bind;
}

DriverAttachments attachmentsOf(const DriverBind& bind, const GuideHierarchy* hierarchy) {
    DriverAttachments out;
    out.byGuide = bind.attached;
    if (hierarchy && !bind.attached.empty()) {
        // A child not attached itself follows its nearest attached ancestor's drivers.
        for (const HierarchyGuide& g : hierarchy->guides()) {
            if (out.byGuide.count(g.id)) continue;
            GuideId up = g.parent;
            for (int depth = 0; up != kNoGuide && depth < 64; ++depth) {
                const auto it = bind.attached.find(up);
                if (it != bind.attached.end()) {
                    out.byGuide[g.id] = it->second;
                    break;
                }
                const HierarchyGuide* parent = hierarchy->guide(up);
                up = parent ? parent->parent : kNoGuide;
            }
        }
    }
    std::uint64_t h = hash64(0xA77AC4);
    for (const auto& [guide, ids] : out.byGuide) {
        h = hashCombine(h, guide);
        for (const std::uint64_t d : ids) h = hashCombine(h, d);
    }
    out.hash = h;
    return out;
}

DriverBind bindDrivers(const GuideSet& drivers, const ScalpMesh* scalp, int frame) {
    DriverBind bind;
    bind.frame = frame;
    const bool onScalp = scalp && scalp->triangleCount() > 0;
    const ScalpMesh rest = onScalp && scalp->hasRest() ? scalp->restPose() : ScalpMesh{};
    for (const Guide& d : drivers.guides()) {
        if (d.points.size() < 2) continue;
        Guide& g = bind.drivers.add(d.id, d.points);
        if (!onScalp) continue;
        std::vector<Vec3f> moved = d.points;
        g.root = rootRigidly(moved, *scalp);  // the anchor alone: the driver itself stays put
        if (!scalp->hasRest()) continue;
        const RootMotion back = rootMotion(rest, *scalp, g.root).inverse();
        if (back.still()) continue;
        for (Vec3f& p : g.points) p = back.point(p);
    }
    return bind;
}

DriversInGraph driversInGraphSpace(const DriverBind& bind, const GuidesPtr& drivers, const ScalpMesh* rest,
                                   const ScalpMesh* now, const ScalpMesh* graph) {
    DriversInGraph out;
    if (bind.empty()) return out;
    const auto bound = std::make_shared<const GuideSet>(bind.drivers);
    out.bound = carried(bound, rest, graph, nullptr);
    std::unordered_map<std::uint64_t, StrandRoot> anchors;
    for (const Guide& d : bind.drivers.guides()) anchors[d.id] = d.root;
    out.now = carried(drivers, now, graph, &anchors);
    return out;
}

std::uint64_t driveBindingKey(const StrandSet& curves, const GuideSet& bound, const DriverAttachments& attached) {
    return hashCombine(hashCombine(curves.contentHash(), bound.contentHash()), attached.hash);
}

DriveBinding bindToDrivers(const StrandSet& curves, const GuideSet& bound, const DriverAttachments& attached) {
    DriveBinding b;
    b.key = driveBindingKey(curves, bound, attached);
    std::unordered_map<std::uint64_t, std::uint32_t> byId;  // a usable bound driver's index by its ID
    for (const std::uint32_t d : usableDrivers(bound)) byId[bound.guides()[d].id] = d;
    const std::size_t count = curves.strandCount();
    b.pointOffset.resize(count + 1, 0);
    for (std::size_t c = 0; c < count; ++c) b.pointOffset[c + 1] = b.pointOffset[c] + curves.strandPointCount(c);
    // Each curve's drivers: the bound ones of the IDs it is attached to.
    std::vector<std::vector<std::uint32_t>> driversOf(count);
    for (std::size_t c = 0; c < count; ++c) {
        const auto it = attached.byGuide.find(curves.ids()[c]);
        if (it == attached.byGuide.end()) continue;
        for (const std::uint64_t id : it->second) {
            const auto d = byId.find(id);
            if (d != byId.end() && std::find(driversOf[c].begin(), driversOf[c].end(), d->second) == driversOf[c].end()) {
                driversOf[c].push_back(d->second);
            }
        }
        b.perPoint = std::max(b.perPoint, std::min(driversOf[c].size(), kMaxDrivers));
    }
    if (b.perPoint == 0) return b;
    b.holds.resize(b.pointOffset[count] * b.perPoint);
    b.weights.assign(count * b.perPoint, 0.0f);

    std::vector<std::vector<Frame>> frames(bound.guides().size());
    for (const auto& [id, d] : byId) boundFrames(bound.guides()[d].points, frames[d]);

    parallelFor(0, count, [&](std::size_t c) {
        const std::size_t n = curves.strandPointCount(c);
        if (n == 0) return;
        if (driversOf[c].empty()) return;  // attached to nothing: it stays
        const Vec3f* points = curves.strandPoints(c);
        // Each of its drivers' mean distance from the curve: the nearest weigh most.
        std::vector<std::pair<float, std::uint32_t>> nearest;
        nearest.reserve(driversOf[c].size());
        float length = 0.0f;
        for (std::size_t i = 1; i < n; ++i) length += (points[i] - points[i - 1]).length();
        for (const std::uint32_t d : driversOf[c]) {
            const std::vector<Vec3f>& line = bound.guides()[d].points;
            float sum = 0.0f;
            for (std::size_t i = 0; i < n; ++i) sum += closestOn(line, points[i]).distance;
            nearest.emplace_back(sum / static_cast<float>(n), d);
        }
        const std::size_t used = std::min(nearest.size(), b.perPoint);
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(used), nearest.end());
        // Shepard weights over its drivers.
        const float eps = std::max(length, 1e-6f) * 1e-4f;
        float total = 0.0f;
        float* w = &b.weights[c * b.perPoint];
        for (std::size_t j = 0; j < used; ++j) {
            const float d = nearest[j].first;
            w[j] = 1.0f / (d * d + eps * eps);
            total += w[j];
        }
        for (std::size_t j = 0; j < used; ++j) w[j] /= total;
        // Each point's place on each of its drivers, never back along it from the point before (a
        // curve running along a driver keeps running along it where it bends close to itself).
        for (std::size_t j = 0; j < used; ++j) {
            const std::uint32_t d = nearest[j].second;
            const std::vector<Vec3f>& line = bound.guides()[d].points;
            const std::vector<Frame>& f = frames[d];
            std::uint32_t lastSegment = 0;
            float lastT = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                Closest at = closestOn(line, points[i]);
                if (i > 0 && (at.segment < lastSegment || (at.segment == lastSegment && at.t < lastT))) {
                    at.segment = lastSegment;
                    at.t = lastT;
                }
                lastSegment = at.segment;
                lastT = at.t;
                DriveBinding::Hold& h = b.holds[(b.pointOffset[c] + i) * b.perPoint + j];
                h.driver = d;
                h.segment = at.segment;
                h.t = at.t;
                const Vec3f offset = points[i] - along(line, at.segment, at.t);
                h.local0 = f[at.segment].toLocal(offset);
                h.local1 = f[at.segment + 1].toLocal(offset);
            }
        }
    });
    return b;
}

void driveCurves(StrandSet& curves, const DriveBinding& binding, const GuideSet& bound, const GuideSet& now,
                 const std::vector<float>& amount, const Ramp& profile) {
    if (binding.perPoint == 0 || curves.strandCount() == 0) return;
    // Each bound driver as it is now (by ID; else as bound) and its frames.
    std::unordered_map<std::uint64_t, std::size_t> byId;
    for (std::size_t d = 0; d < now.guides().size(); ++d) byId[now.guides()[d].id] = d;
    const std::size_t drivers = bound.guides().size();
    std::vector<const std::vector<Vec3f>*> lines(drivers, nullptr);
    std::vector<std::vector<Frame>> frames(drivers);
    std::vector<char> used(drivers, 0);
    for (const DriveBinding::Hold& h : binding.holds) used[h.driver] = 1;
    for (std::size_t d = 0; d < drivers; ++d) {
        if (!used[d]) continue;
        const Guide& was = bound.guides()[d];
        const auto it = byId.find(was.id);
        const bool moved = it != byId.end() && now.guides()[it->second].points.size() == was.points.size();
        lines[d] = moved ? &now.guides()[it->second].points : &was.points;
        std::vector<Frame> bf;
        boundFrames(was.points, bf);
        if (moved) {
            currentFrames(*lines[d], bf, frames[d]);
        } else {
            frames[d] = std::move(bf);
        }
    }
    curves.mutablePositions();
    const std::size_t per = binding.perPoint;
    parallelFor(0, curves.strandCount(), [&](std::size_t c) {
        const float a = c < amount.size() ? amount[c] : 1.0f;
        const float* w = &binding.weights[c * per];
        float influence = 0.0f;
        for (std::size_t j = 0; j < per; ++j) influence += w[j];
        if (a == 0.0f || influence <= 0.0f) return;
        const std::size_t n = curves.strandPointCount(c);
        Vec3f* points = curves.strandPoints(c);
        for (std::size_t i = 1; i < n; ++i) {  // the root stays on the scalp
            const float share = a * profile(static_cast<float>(i) / static_cast<float>(n - 1));
            if (share == 0.0f) continue;
            Vec3f move;
            for (std::size_t j = 0; j < per; ++j) {
                if (w[j] <= 0.0f) continue;
                const DriveBinding::Hold& h = binding.holds[(binding.pointOffset[c] + i) * per + j];
                const std::vector<Vec3f>& line = *lines[h.driver];
                const std::vector<Frame>& f = frames[h.driver];
                const Vec3f offset = f[h.segment].toWorld(h.local0) * (1.0f - h.t) + f[h.segment + 1].toWorld(h.local1) * h.t;
                move += (along(line, h.segment, h.t) + offset - points[i]) * w[j];
            }
            points[i] += move * share;
        }
    });
}

}  // namespace groomlab
