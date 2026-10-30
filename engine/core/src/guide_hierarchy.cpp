// SPDX-License-Identifier: Apache-2.0
#include "groomlab/guide_hierarchy.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "schema_internal.h"

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.28318530718f;


// Linear sample of values placed evenly on t in [0, 1].
template <class T>
T sampleEven(const std::vector<T>& values, float t, T fallback) {
    if (values.empty()) return fallback;
    if (values.size() == 1) return values[0];
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(values.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(x), values.size() - 2);
    const float f = x - static_cast<float>(i);
    return values[i] * (1.0f - f) + values[i + 1] * f;
}

float paramAt(std::size_t i, std::size_t n) { return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f; }

// A per-point layer resampled along the guide to n points (the values sit
// evenly along it, as a child's points sit on its parent's).
std::vector<float> resampleLayer(const std::vector<float>& values, std::size_t n) {
    if (values.size() == n) return values;
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = sampleEven(values, paramAt(i, n), 0.0f);
    return out;
}

// Per-point layers resampled to the guide's point count.
void fitLayers(HierarchyGuide& g) {
    for (auto& [name, values] : g.pointAttributes) {
        if (values.size() != g.points.size()) values = resampleLayer(values, g.points.size());
    }
}

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The root footprint a shaped tube's children root on (GuideHierarchy::resolveFollower): the
// parent's root section turned from its root direction toward the scalp's normal there (the
// smallest turn, as TubeSpine::setRootSurface lays it for strands), all of the way for a parent
// leaning kSlanted or more from the normal, none up to kUpright (its children snapped onto the
// scalp from the root section, as always), smoothly between. A child's shift onto it fades out
// along the parent by (1 - t / join)^2, join a few root extents (TubeSpine::rootJoin's).
struct FootprintTurn {
    bool active = false;
    Rotation turn, back;  // section to footprint, and back
    float join = 1.0f;
    float fade(float t) const {
        if (!active) return 0.0f;
        const float f = std::max(1.0f - t / join, 0.0f);
        return f * f;
    }
};

FootprintTurn footprintTurn(const HierarchyGuide& parent, const Frame& rootFrame, float scale) {
    FootprintTurn out;
    if (!parent.tube.shaped() || !(parent.rootNormal.length() > 0.0f) || parent.points.size() < 2) return out;
    constexpr float kUpright = 0.5235988f, kSlanted = 1.0471976f;  // 30 and 60 degrees
    const Vec3f normal = parent.rootNormal.normalized();
    const Vec3f tangent = rootFrame.tangent;
    const float c = std::clamp(tangent.dot(normal), -1.0f, 1.0f);
    const float w = smoothstep01((std::acos(c) - kUpright) / (kSlanted - kUpright));
    if (!(w > 0.0f) || c < -1.0f + 1e-4f) return out;
    const Vec3f target = (tangent * (1.0f - w) + normal * w).normalized();
    out.turn = Rotation(tangent, target);
    out.back = Rotation(target, tangent);
    const TubeSection root = parent.tube.sectionAt(0.0f);
    const float extent = root.width * scale * std::max(1.0f, root.aspect);
    float length = 0.0f;
    for (std::size_t i = 1; i < parent.points.size(); ++i) length += (parent.points[i] - parent.points[i - 1]).length();
    constexpr float kJoinExtents = 3.0f;
    out.join = length > 0.0f ? std::clamp(kJoinExtents * extent / length, 1e-3f, 1.0f) : 1.0f;
    out.active = true;
    return out;
}

// A shaped tube's roll on the scalp (TubeProfile::roll): per guide point, the angle its sections
// turn from the guide's rotation-minimizing frames (`frames`) so that, at twist 0, the section's
// height (v) points off the scalp and its width (u) lies along it, all along the guide: the
// scalp's normal at the point nearest each guide point, taken across the guide. It holds
// where the guide lies within kNear section extents of the scalp and leans at least 30 degrees
// off the normal there; farther off (none past kFar extents) or more upright, the sections keep
// the roll they had before (the frames carried on), so a lock hanging off the head keeps the
// way it left it. Evened out along the guide; empty when it is 0 all along.
std::vector<float> sectionRoll(const std::vector<Vec3f>& points, const std::vector<Frame>& frames, const TubeProfile& tube,
                               const ScalpMesh& scalp, float scale) {
    constexpr float kNear = 2.0f, kFar = 6.0f;
    constexpr float kPi = 3.14159265358979f;
    const std::size_t n = points.size();
    std::vector<float> roll(n, 0.0f);
    float last = 0.0f;
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        const float t = paramAt(i, n);
        const float width = tube.widthAt(t) * scale;
        const float extent = std::max(width, width * tube.aspectAt(t));
        float target = last, weight = 0.0f;
        if (extent > 0.0f) {
            const SurfacePoint sp = scalp.closestPoint(points[i], kFar * extent);
            if (sp.root.face >= 0) {
                const Vec3f nrm = scalp.normal(sp.root);
                const Vec3f& tan = frames[i].tangent;
                const Vec3f across = nrm - tan * tan.dot(nrm);
                const float len = across.length();
                const float nl = std::max(nrm.length(), 1e-12f);
                if (len > 1e-4f * nl) {
                    const float d = (sp.position - points[i]).length();
                    weight = (1.0f - smoothstep01((d - kNear * extent) / ((kFar - kNear) * extent))) *
                             smoothstep01(len / nl / 0.5f);
                    // The angle turning the binormal onto `across` about the tangent (twist's way:
                    // v = cos * binormal - sin * normal).
                    target = std::atan2(-across.dot(frames[i].normal), across.dot(frames[i].binormal));
                    while (target - last > kPi) target -= 2.0f * kPi;  // the nearest turn to the last
                    while (target - last < -kPi) target += 2.0f * kPi;
                }
            }
        }
        roll[i] = last + (target - last) * weight;
        last = roll[i];
        any = any || std::abs(roll[i]) > 1e-6f;
    }
    if (!any) return {};
    for (int pass = 0; pass < 2; ++pass) {  // evened out, the root's kept
        std::vector<float> next = roll;
        for (std::size_t i = 1; i < n; ++i) next[i] = (roll[i - 1] + 2.0f * roll[i] + roll[std::min(i + 1, n - 1)]) * 0.25f;
        roll.swap(next);
    }
    return roll;
}

Vec3f rotateXY(const Vec3f& v, float angle) {
    const float c = pm::cos(angle), s = pm::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

Vec3f rootTangent(const std::vector<Vec3f>& pts) {
    return pts.size() >= 2 ? (pts[1] - pts[0]).normalized() : Vec3f(0, 1, 0);
}

// The way across a guide along the scalp (normal n at its root): perpendicular to n and to the
// way the guide lies over the scalp (root to tip, else its root direction, taken into the scalp's
// tangent plane), turned so a frame started from it has its binormal off the scalp. `fallback`
// for a guide standing straight off the scalp.
Vec3f acrossScalp(const std::vector<Vec3f>& pts, const Vec3f& n, const Vec3f& fallback) {
    if (pts.size() < 2 || !(n.length() > 0.0f)) return fallback;
    const Vec3f unit = n.normalized();
    for (const Vec3f& way : {pts.back() - pts.front(), pts[1] - pts[0]}) {
        const Vec3f flat = way - unit * way.dot(unit);
        if (flat.length() > 1e-3f * way.length() && flat.length() > 1e-12f) return unit.cross(flat).normalized();
    }
    return fallback;
}

// Surface frame at a root: normal = the smooth surface normal (local y),
// the root face's first edge made perpendicular to it (local x), and their
// cross product (local z).
Frame rootFrame(const ScalpMesh& scalp, const StrandRoot& root) {
    const auto& tri = scalp.triangles()[root.face];
    const Vec3f n = scalp.normal(root);
    Vec3f edge = scalp.points()[tri[1]] - scalp.points()[tri[0]];
    edge = edge - n * edge.dot(n);
    const Vec3f e1 = edge.length() > 1e-9f ? edge.normalized() : anyPerpendicular(n);
    Frame f;
    f.normal = e1;              // local x
    f.binormal = n;             // local y
    f.tangent = e1.cross(n);    // local z (so binormal = tangent x normal)
    return f;
}

bool isBound(const HierarchyGuide& g, const ScalpMesh& scalp) {
    return g.root.face >= 0 && static_cast<std::size_t>(g.root.face) < scalp.triangleCount() &&
           !g.rootLocal.empty() && g.rootLocal.size() == g.points.size();
}

// `scale`: the size shown (GuideHierarchy::setScale): the world shape is that times what is stored.
void bind(HierarchyGuide& g, const ScalpMesh& scalp, float scale) {
    const Frame f = rootFrame(scalp, g.root);
    const Vec3f origin = scalp.position(g.root);
    g.rootLocal.resize(g.points.size());
    for (std::size_t i = 0; i < g.points.size(); ++i) g.rootLocal[i] = f.toLocal(g.points[i] - origin) * (1.0f / scale);
    g.frameUpLocal = f.toLocal(g.frameUp);
}

void rebuildFromRoot(HierarchyGuide& g, const ScalpMesh& scalp, float scale) {
    const Frame f = rootFrame(scalp, g.root);
    const Vec3f origin = scalp.position(g.root);
    for (std::size_t i = 0; i < g.points.size(); ++i) g.points[i] = origin + f.toWorld(g.rootLocal[i] * scale);
    g.frameUp = f.toWorld(g.frameUpLocal);
}


std::uint64_t hashVec(std::uint64_t h, const Vec3f& v) { return hashFloat(hashFloat(hashFloat(h, v.x), v.y), v.z); }

}  // namespace

// ---------------------------------------------------------------------------


std::vector<Frame> HierarchyGuide::frames() const {
    return rotationMinimizingFrames(points.data(), points.size(), frameUp);
}

// ---------------------------------------------------------------------------
// Groups

void GuideHierarchy::setGroupColor(GroupId id, const std::array<float, 3>& linearRgb) {
    for (GuideGroup& g : groups_) {
        if (g.id == id) {
            g.hasColor = true;
            g.color = linearRgb;
            return;
        }
    }
    throw std::invalid_argument("setGroupColor: unknown group");
}

void GuideHierarchy::setGroupSegments(GroupId id, int segments) {
    if (segments < 0 || segments > 256) throw std::invalid_argument("setGroupSegments: segments are 0 to 256");
    for (GuideGroup& g : groups_) {
        if (g.id == id) {
            if (g.segments != segments) invalidateHash();
            g.segments = segments;
            return;
        }
    }
    throw std::invalid_argument("setGroupSegments: unknown group");
}

std::vector<Vec3f> GuideHierarchy::withGroupSegments(GroupId id, std::vector<Vec3f> points) const {
    const GuideGroup* g = group(id);
    if (!g || g->segments < 1 || points.size() < 2) return points;
    return resampleSmoothCurve(points.data(), points.size(), static_cast<std::size_t>(g->segments) + 1);
}

std::size_t GuideHierarchy::resample(const std::vector<GuideId>& ids, int segments, bool groupWide) {
    if (segments < 1 || segments > 256) throw std::invalid_argument("resample: segments are 1 to 256");
    std::size_t count = 0;
    for (const GuideId id : ids) {
        const HierarchyGuide* g = guide(id);
        if (!g) throw std::invalid_argument("resample: no guide " + std::to_string(id));
        float frozen = 0.0f;
        if (g->points.size() < 2 || (effectiveAttribute(*g, "freeze", &frozen) && frozen >= 0.5f)) continue;
        if (groupWide && g->ownSegments) continue;
        if (!groupWide) {
            const GuideGroup* grp = group(g->group);
            const bool own = !grp || grp->segments != segments;
            if (own != g->ownSegments) invalidateHash();
            mutableGuide(id)->ownSegments = own;
        }
        setPoints(id, resampleSmoothCurve(g->points.data(), g->points.size(), static_cast<std::size_t>(segments) + 1));
        ++count;
    }
    return count;
}

int GuideHierarchy::resetGroupSegments(GroupId id) {
    const GuideGroup* grp = group(id);
    if (!grp) throw std::invalid_argument("resetGroupSegments: unknown group");
    const std::vector<GuideId> ids = guidesInGroup(id);
    int segments = grp->segments;
    if (segments < 1) {
        // The most common count, of the guides that follow the group (else of all of them).
        std::map<int, std::size_t> counts, all;
        for (const GuideId g : ids) {
            const HierarchyGuide& guide = *this->guide(g);
            if (guide.points.size() < 2) continue;
            const int n = static_cast<int>(guide.points.size()) - 1;
            ++all[n];
            if (!guide.ownSegments) ++counts[n];
        }
        const std::map<int, std::size_t>& from = counts.empty() ? all : counts;
        if (from.empty()) return 0;
        segments = std::max_element(from.begin(), from.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
        setGroupSegments(id, std::min(segments, 256));
        segments = std::min(segments, 256);
    }
    std::vector<GuideId> follow;
    for (const GuideId g : ids) {
        float frozen = 0.0f;
        const HierarchyGuide& guide = *this->guide(g);
        if (effectiveAttribute(guide, "freeze", &frozen) && frozen >= 0.5f) continue;
        if (guide.ownSegments) {
            invalidateHash();
            mutableGuide(g)->ownSegments = false;
        }
        follow.push_back(g);
    }
    resample(follow, segments, true);
    return segments;
}

GroupId GuideHierarchy::addGroup(const std::string& name, GroupId parent) {
    invalidateHash();
    if (parent != kNoGroup && !group(parent)) throw std::invalid_argument("addGroup: unknown parent group");
    if (findGroup(name) != kNoGroup) throw std::invalid_argument("addGroup: group '" + name + "' exists");
    groups_.push_back({nextGroupId_++, name, parent});
    return groups_.back().id;
}

const GuideGroup* GuideHierarchy::group(GroupId id) const {
    for (const GuideGroup& g : groups_) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

GroupId GuideHierarchy::findGroup(const std::string& name) const {
    for (const GuideGroup& g : groups_) {
        if (g.name == name) return g.id;
    }
    return kNoGroup;
}

int GuideHierarchy::groupDepth(GroupId id) const {
    int depth = -1;
    for (const GuideGroup* g = group(id); g; g = group(g->parent)) {
        ++depth;
        if (depth > static_cast<int>(groups_.size())) return -1;  // cycle guard
    }
    return depth;
}

int GuideHierarchy::maxDepth() const {
    int depth = -1;
    for (const GuideGroup& g : groups_) depth = std::max(depth, groupDepth(g.id));
    return depth;
}

std::set<GroupId> GuideHierarchy::deepestGroups() const {
    std::set<GroupId> holding, above;
    for (const HierarchyGuide& g : guides_) {
        if (g.points.size() >= 2) holding.insert(g.group);
    }
    for (const GroupId id : holding) {
        const GuideGroup* grp = group(id);
        for (int depth = 0; grp && grp->parent != kNoGroup && depth < 64; ++depth) {
            above.insert(grp->parent);
            grp = group(grp->parent);
        }
    }
    std::set<GroupId> out;
    for (const GroupId id : holding) {
        if (!above.count(id)) out.insert(id);
    }
    return out;
}

void GuideHierarchy::renameGroup(GroupId id, const std::string& name) {
    invalidateHash();
    if (name.empty()) throw std::invalid_argument("a guide group needs a name");
    const GroupId existing = findGroup(name);
    if (existing != kNoGroup && existing != id) throw std::invalid_argument("a guide group named '" + name + "' exists");
    for (GuideGroup& g : groups_) {
        if (g.id == id) {
            g.name = name;
            return;
        }
    }
    throw std::invalid_argument("no such guide group");
}

void GuideHierarchy::removeGroup(GroupId id) {
    invalidateHash();
    std::set<GroupId> doomed{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const GuideGroup& g : groups_) {
            if (doomed.count(g.parent) && doomed.insert(g.id).second) grew = true;
        }
    }
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const GuideGroup& g) { return doomed.count(g.id) > 0; }),
                  groups_.end());
    guides_.erase(std::remove_if(guides_.begin(), guides_.end(),
                                 [&](const HierarchyGuide& g) { return doomed.count(g.group) > 0; }),
                  guides_.end());
    rebuildIndex();
}

// ---------------------------------------------------------------------------
// Guides

void GuideHierarchy::rebuildIndex() {
    invalidateHash();
    index_.clear();
    for (std::size_t i = 0; i < guides_.size(); ++i) index_[guides_[i].id] = i;
}

GuideId GuideHierarchy::nextGuideId() {
    invalidateHash();
    while (index_.count(nextGuideId_)) ++nextGuideId_;
    return nextGuideId_++;
}

const HierarchyGuide* GuideHierarchy::guide(GuideId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &guides_[it->second];
}

HierarchyGuide* GuideHierarchy::mutableGuide(GuideId id) {
    invalidateHash();
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &guides_[it->second];
}

GuideId GuideHierarchy::addGuide(GroupId groupId, std::vector<Vec3f> points, TubeProfile tube, GuideId id) {
    invalidateHash();
    const GuideGroup* g = group(groupId);
    if (!g) throw std::invalid_argument("addGuide: unknown group");
    if (g->parent != kNoGroup) throw std::invalid_argument("addGuide: primary guides belong in root groups");
    if (id == kNoGuide) id = nextGuideId();
    if (index_.count(id)) throw std::invalid_argument("addGuide: duplicate guide id");

    HierarchyGuide guide;
    guide.id = id;
    guide.group = groupId;
    guide.tube = std::move(tube);
    adoptTubeShape(guide.tube);
    guide.frameUp = anyPerpendicular(rootTangent(points));
    guide.points = std::move(points);
    index_[id] = guides_.size();
    guides_.push_back(std::move(guide));
    return id;
}

std::vector<GuideId> GuideHierarchy::addGuides(GroupId groupId, const GuideSet& set, TubeProfile tube) {
    invalidateHash();
    std::vector<GuideId> added;
    for (const Guide& g : set.guides()) {
        if (g.points.size() < 2) continue;
        const GuideId wanted = g.id != kNoGuide && !index_.count(g.id) ? g.id : kNoGuide;
        TubeProfile own = tube;
        if (g.hasTube) {  // a hierarchy's guide carries its tube, at the size shown there
            own = g.tube;
            own.scaleWidth(1.0f / scale_);
        }
        const GuideId id = addGuide(groupId, g.points, std::move(own), wanted);
        HierarchyGuide* h = mutableGuide(id);
        h->attributes = g.attributes;
        h->pointAttributes = g.pointAttributes;
        fitLayers(*h);
        h->root = g.root;  // kept when the set was rooted on the same scalp; re-rooted otherwise
        added.push_back(id);
    }
    return added;
}

std::vector<GuideId> GuideHierarchy::guidesInGroup(GroupId groupId) const {
    std::vector<GuideId> ids;
    for (const HierarchyGuide& g : guides_) {
        if (g.group == groupId) ids.push_back(g.id);
    }
    return ids;
}

std::vector<GuideId> GuideHierarchy::childrenOf(GuideId parent) const {
    std::vector<GuideId> ids;
    for (const HierarchyGuide& g : guides_) {
        if (g.parent == parent) ids.push_back(g.id);
    }
    return ids;
}

GroupId GuideHierarchy::generateChildren(GroupId parentGroup, const std::string& childGroupName,
                                         const ChildGenerationParams& params, const std::vector<GuideId>* parents) {
    invalidateHash();
    if (!group(parentGroup)) throw std::invalid_argument("generateChildren: unknown parent group");
    std::vector<GuideId> chosen = parents ? *parents : guidesInGroup(parentGroup);
    for (const GuideId id : chosen) {
        const HierarchyGuide* g = guide(id);
        if (!g || g->group != parentGroup) {
            throw std::invalid_argument("generateChildren: guide " + std::to_string(id) + " is not in the parent group");
        }
    }
    GroupId childGroup = findGroup(childGroupName);
    if (childGroup == kNoGroup) {
        childGroup = addGroup(childGroupName, parentGroup);
    } else if (group(childGroup)->parent != parentGroup) {
        throw std::invalid_argument("generateChildren: group '" + childGroupName + "' belongs to another parent group");
    } else {
        // Regenerating: the chosen parents' current children there make way.
        const std::set<GuideId> regenerate(chosen.begin(), chosen.end());
        std::vector<GuideId> old;
        for (const HierarchyGuide& g : guides_) {
            if (g.group == childGroup && regenerate.count(g.parent)) old.push_back(g.id);
        }
        if (!old.empty()) removeGuides(old);
    }

    std::vector<GuideId> made;
    for (const GuideId parentId : chosen) {
        // The parent's frames, once for all its children (a shaped tube's children start theirs from its root's).
        const std::vector<Frame> parentFrames = guide(parentId)->frames();
        for (std::size_t k = 0; k < params.count; ++k) {
            const HierarchyGuide& parent = *guide(parentId);  // re-fetch: guides_ grows below
            const std::size_t n = parent.points.size();
            if (n < 2) break;

            GuideId id = hashCombine(hashCombine(hash64(parentId), params.seed), k);
            while (id == kNoGuide || index_.count(id)) id = hash64(id);  // keep IDs stable, unique and nonzero

            Rng rng(id);
            const float u = rng.nextFloat();
            const float angle = kTwoPi * rng.nextFloat();
            const Vec3f dir(pm::cos(angle), pm::sin(angle), 0.0f);
            Vec3f rootXY = dir * (params.rootSpread * std::sqrt(u));
            // Drawn y first, then x, one statement each: C++ leaves the order of a call's
            // arguments to the compiler, and this is the order MSVC gave, kept everywhere.
            const float jitterY = rng.uniform(-1, 1);
            const float jitterX = rng.uniform(-1, 1);
            Vec3f tipXY = dir * (params.tipSpread * std::sqrt(u)) + Vec3f(jitterX, jitterY, 0.0f) * params.jitter;
            if (!parent.tube.roundShape()) {
                // Inside the shape's outline (uniform over its area) rather than a disc.
                const SectionPoint p = randomPointInTubeShape(parent.tube, hashCombine(id, 0x7B5Eu));
                const Vec3f spot(p.u, p.v, 0.0f);
                rootXY = spot * params.rootSpread;
                tipXY = spot * params.tipSpread + Vec3f(jitterX, jitterY, 0.0f) * params.jitter;
            }

            HierarchyGuide child;
            child.id = id;
            child.group = childGroup;
            child.parent = parentId;
            child.tube = parent.tube;
            child.tube.scaleWidth(params.childTubeScale);
            child.offsets.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                const float t = paramAt(i, n);
                child.offsets[i] = rotateXY(rootXY * (1.0f - t) + tipXY * t, params.twist * t);
            }
            resolveFollower(child, parent, nullptr, &parentFrames);
            if (!parent.tube.shaped()) child.frameUp = anyPerpendicular(rootTangent(child.points));

            index_[id] = guides_.size();
            guides_.push_back(std::move(child));
            made.push_back(id);
        }
    }
    // A child group with segments of its own: the children rebuilt to them (their parents' count
    // otherwise).
    if (group(childGroup)->segments > 0) {
        for (const GuideId id : made) setPoints(id, withGroupSegments(childGroup, guide(id)->points));
    }
    return childGroup;
}

// ---------------------------------------------------------------------------
// Evaluation and editing

GuideId GuideHierarchy::addChildGuide(GroupId groupId, GuideId parentId, std::vector<Vec3f> points,
                                      std::optional<TubeProfile> tube) {
    invalidateHash();
    const GuideGroup* g = group(groupId);
    if (!g || g->parent == kNoGroup) throw std::invalid_argument("addChildGuide: not a child group");
    const HierarchyGuide* parent = guide(parentId);
    if (!parent || parent->group != g->parent) {
        throw std::invalid_argument("addChildGuide: the parent must be in the group's parent group");
    }
    if (points.size() < 2) throw std::invalid_argument("addChildGuide: a guide needs at least 2 points");
    HierarchyGuide child;
    child.id = nextGuideId();
    child.group = groupId;
    child.parent = parentId;
    if (tube && !tube->width.empty()) {
        child.tube = std::move(*tube);
        adoptTubeShape(child.tube);
    } else {
        child.tube = parent->tube;
        child.tube.scaleWidth(0.35f);
    }
    // A shaped parent's children start their frames from its root's (resolveFollower keeps them so).
    child.frameUp = parent->tube.shaped() ? parent->frames().front().normal : anyPerpendicular(rootTangent(points));
    child.points = std::move(points);
    bindOffsets(child, *parent);
    const GuideId id = child.id;
    index_[id] = guides_.size();
    guides_.push_back(std::move(child));
    return id;
}

void GuideHierarchy::resolveFollower(HierarchyGuide& child, const HierarchyGuide& parent, const ScalpMesh* scalp,
                                     const std::vector<Frame>* parentFrames) {
    invalidateHash();
    const std::size_t n = parent.points.size();
    std::vector<Frame> computed;
    if (!parentFrames || parentFrames->size() != n) computed = parent.frames();
    const std::vector<Frame>& frames = computed.empty() ? *parentFrames : computed;
    child.points.resize(n);
    // The offsets are the parent tube's section coordinates (tube.h): across in its sections,
    // along in scene units. A plain round tube's sections (no twist, aspect 1) give the offsets
    // times the width, to the bit as before sections.
    for (std::size_t i = 0; i < n; ++i) {
        const float t = paramAt(i, n);
        const Vec3f off = sampleEven(child.offsets, t, Vec3f{});
        const Vec3f o = sectionOffset(parent.tube.sectionAt(t), off.x, off.y, scale_);
        child.points[i] = parent.points[i] + frames[i].toWorld({o.x, o.y, off.z * scale_});
    }
    // A shaped tube's children keep their frames started from its root's, so their own tubes lie
    // as the parent's does however it turns.
    if (parent.tube.shaped() && n > 0) child.frameUp = frames[0].normal;

    // A shaped tube's parent leaving the scalp at a slant: its children's roots on its root
    // footprint (footprintTurn), not snapped straight down from its root section.
    if (scalp && scalp->triangleCount() > 0 && n >= 2) {
        const FootprintTurn foot = footprintTurn(parent, frames[0], scale_);
        if (foot.active) {
            const Vec3f rootOffset = child.points[0] - parent.points[0];
            const Vec3f shift = foot.turn(rootOffset) - rootOffset;
            for (std::size_t i = 0; i < n; ++i) child.points[i] += shift * foot.fade(paramAt(i, n));
        }
    }

    // The cross-section at the root is not the scalp surface: snap the root
    // onto the scalp and fade the correction out toward the tip.
    if (scalp && scalp->triangleCount() > 0 && n > 0) {
        // The root sits in the parent's tube, around the parent's root on the scalp: the scalp
        // is no farther than that point, so the search is bounded by it. A bounded search that
        // finds a point finds the nearest (the same as unbounded); one that finds none (the
        // bound fell short) searches again unbounded.
        SurfacePoint sp;
        if (scalp->contains(parent.root)) {
            const float d = (scalp->position(parent.root) - child.points[0]).length();
            sp = scalp->closestPoint(child.points[0], d * 1.01f + 1e-6f);
        }
        if (sp.root.face < 0) sp = scalp->closestPoint(child.points[0]);
        const Vec3f delta = sp.position - child.points[0];
        for (std::size_t i = 0; i < n; ++i) {
            const float w = 1.0f - paramAt(i, n);
            child.points[i] += delta * (w * w);
        }
        child.root = sp.root;
    }
    // Its own point count: the shape respaced along its smooth curve (the root stays where it is).
    if (child.pointCount >= 2 && child.pointCount != n && n >= 2) {
        child.points = resampleSmoothCurve(child.points.data(), n, child.pointCount);
    }
}

std::vector<Vec3f> GuideHierarchy::offsetsFromWorld(const HierarchyGuide& child, const HierarchyGuide& parent) const {
    const std::size_t n = parent.points.size();
    const std::vector<Vec3f> pts = child.points.size() == n
                                       ? child.points
                                       : resampleByArcLength(child.points.data(), child.points.size(), n);
    const std::vector<Frame> frames = parent.frames();
    // A child on its parent's root footprint (resolveFollower): the footprint's shift taken back
    // out, so the offsets give the same shape again.
    Vec3f shift;
    const FootprintTurn foot = n >= 2 ? footprintTurn(parent, frames[0], scale_) : FootprintTurn{};
    if (foot.active) {
        const Vec3f onFoot = pts[0] - parent.points[0];
        shift = onFoot - foot.back(onFoot);
    }
    std::vector<Vec3f> offsets(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = paramAt(i, n);
        const Vec3f local = frames[i].toLocal(pts[i] - shift * foot.fade(t) - parent.points[i]);
        const SectionPoint at = sectionCoords(parent.tube.sectionAt(t), local.x, local.y, scale_);
        offsets[i] = {at.u, at.v, local.z / scale_};
    }
    return offsets;
}

void GuideHierarchy::bindOffsets(HierarchyGuide& child, const HierarchyGuide& parent) const {
    child.offsets = offsetsFromWorld(child, parent);
    const std::size_t own = child.points.size();
    child.pointCount = own >= 2 && own != parent.points.size() ? static_cast<std::uint32_t>(own) : 0;
}

void GuideHierarchy::evaluateGuide(HierarchyGuide& g, const ScalpMesh* scalp, const std::vector<Frame>* parentFrames) {
    invalidateHash();
    placeGuide(g, scalp, parentFrames);
    // What its children and its tube read of the scalp: the normal at its root (footprintTurn)
    // and, for a shaped tube, its sections' roll (TubeProfile::roll).
    if (!scalp || scalp->triangleCount() == 0) return;
    if (scalp->contains(g.root)) g.rootNormal = scalp->normal(g.root);
    if (g.tube.shaped() && g.points.size() >= 2) {
        g.tube.roll = sectionRoll(g.points, rotationMinimizingFrames(g.points.data(), g.points.size(), g.frameUp), g.tube,
                                  *scalp, scale_);
    } else {
        g.tube.roll.clear();
    }
}

void GuideHierarchy::placeGuide(HierarchyGuide& g, const ScalpMesh* scalp, const std::vector<Frame>* parentFrames) {
    if (g.followsParent()) {
        if (const HierarchyGuide* parent = guide(g.parent)) resolveFollower(g, *parent, scalp, parentFrames);
        return;
    }
    if (!scalp || scalp->triangleCount() == 0 || g.points.size() < 2) return;
    if (isBound(g, *scalp)) {
        rebuildFromRoot(g, *scalp, scale_);
        if (g.layFrame) {  // its tube given a shape (setTube): its frame laid across the scalp
            g.frameUp = acrossScalp(g.points, scalp->normal(g.root), g.frameUp);
            g.frameUpLocal = rootFrame(*scalp, g.root).toLocal(g.frameUp);
            g.layFrame = false;
        }
        return;
    }
    // Never-rooted primaries accept curves drawn tip-first. Edited guides
    // keep their root, so a combed guide lying on the scalp never flips; a
    // curve with both ends on the scalp (a braid path) roots where it starts.
    const bool firstRooting = !g.isChild() && g.root.face < 0;
    if (firstRooting && drawnTipFirst(g.points, *scalp)) {
        std::reverse(g.points.begin(), g.points.end());
        g.frameUp = anyPerpendicular(rootTangent(g.points));
    }
    // An edited guide whose first point is still its root's spot (a brush moved the rest of it)
    // keeps that root as it is: found again, the closest point's rounding would move the whole
    // guide by a hair each edit.
    const bool rootStays = !firstRooting && scalp->contains(g.root) && scalp->position(g.root) == g.points.front();
    if (!rootStays) g.root = rootRigidly(g.points, *scalp);
    // A new primary with a shaped tube (one it took from its neighbours) starts its frame across
    // the scalp, so the tube lies flat on the head at twist 0 (a ribbon's width along the scalp,
    // its height off it) however it leaves the scalp.
    if (firstRooting && (g.tube.shaped() || g.layFrame) && scalp->contains(g.root)) {
        g.frameUp = acrossScalp(g.points, scalp->normal(g.root), g.frameUp);
    }
    g.layFrame = false;
    bind(g, *scalp, scale_);
}

void GuideHierarchy::setScale(float scale) {
    if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;
    if (scale == scale_) return;
    invalidateHash();
    scale_ = scale;
}

std::vector<std::size_t> GuideHierarchy::depthOrder(const std::vector<std::size_t>& indices) const {
    std::unordered_map<GroupId, int> depth;
    for (const GuideGroup& g : groups_) depth[g.id] = groupDepth(g.id);
    std::vector<std::size_t> order = indices;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return depth[guides_[a].group] < depth[guides_[b].group];
    });
    return order;
}

void GuideHierarchy::evaluate(const ScalpMesh* scalp) {
    invalidateHash();
    if (scalp && scalp->triangleCount() > 0) {
        const std::uint64_t topology = scalp->topologyHash();
        if (boundTopology_ != 0 && boundTopology_ != topology) {
            // Another mesh: the root frames mean nothing here. Bound guides keep
            // their last world shape and re-root below (their roots are not
            // cleared, so no primary is taken for a tip-first curve).
            for (HierarchyGuide& g : guides_) {
                if (!g.followsParent()) g.rootLocal.clear();
            }
        }
        boundTopology_ = topology;
    }
    // Parents before children.
    std::vector<std::size_t> all(guides_.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
    evaluateInOrder(depthOrder(all), scalp);
}

void GuideHierarchy::evaluateInOrder(const std::vector<std::size_t>& order, const ScalpMesh* scalp) {
    invalidateHash();
    // Parents come first, so a parent's frames are final by the time its
    // followers need them: compute them once per parent, not once per child.
    std::unordered_map<GuideId, std::vector<Frame>> frames;
    auto framesOf = [&](const HierarchyGuide& g) -> const std::vector<Frame>* {
        if (!g.followsParent()) return nullptr;
        const HierarchyGuide* parent = guide(g.parent);
        if (!parent) return nullptr;
        auto it = frames.find(parent->id);
        if (it == frames.end()) it = frames.emplace(parent->id, parent->frames()).first;
        return &it->second;
    };
    // `order` runs level by level (group depth, as depthOrder sorts it). A guide reads only its
    // parent, a level up, so a level's guides are evaluated side by side, each as it would be
    // alone; a level holding a guide's parent too (a hierarchy whose groups disagree with its
    // guides) runs one guide at a time, in order.
    constexpr std::size_t kParallelLevel = 64;
    std::unordered_map<GroupId, int> depth;
    for (const GuideGroup& g : groups_) depth[g.id] = groupDepth(g.id);
    std::size_t begin = 0;
    while (begin < order.size()) {
        const int level = depth[guides_[order[begin]].group];
        std::size_t end = begin + 1;
        while (end < order.size() && depth[guides_[order[end]].group] == level) ++end;
        bool together = end - begin >= kParallelLevel;
        if (together) {
            std::unordered_set<GuideId> inLevel;
            for (std::size_t k = begin; k < end; ++k) inLevel.insert(guides_[order[k]].id);
            for (std::size_t k = begin; k < end && together; ++k) {
                const HierarchyGuide& g = guides_[order[k]];
                together = !(g.followsParent() && inLevel.count(g.parent));
            }
        }
        if (!together) {
            for (std::size_t k = begin; k < end; ++k) {
                HierarchyGuide& g = guides_[order[k]];
                evaluateGuide(g, scalp, framesOf(g));
            }
        } else {
            // The parents' frames first (from the parents as they are now: none of them is in
            // this level), then every guide of the level.
            std::vector<const HierarchyGuide*> parents;
            std::unordered_set<GuideId> listed;
            for (std::size_t k = begin; k < end; ++k) {
                const HierarchyGuide& g = guides_[order[k]];
                if (!g.followsParent()) continue;
                const HierarchyGuide* parent = guide(g.parent);
                if (parent && !frames.count(parent->id) && listed.insert(parent->id).second) parents.push_back(parent);
            }
            std::vector<std::vector<Frame>> made(parents.size());
            parallelFor(0, parents.size(), [&](std::size_t p) { made[p] = parents[p]->frames(); }, 16);
            for (std::size_t p = 0; p < parents.size(); ++p) frames.emplace(parents[p]->id, std::move(made[p]));
            std::vector<const std::vector<Frame>*> parentFrames(end - begin);
            for (std::size_t k = begin; k < end; ++k) parentFrames[k - begin] = framesOf(guides_[order[k]]);
            parallelFor(begin, end, [&](std::size_t k) { evaluateGuide(guides_[order[k]], scalp, parentFrames[k - begin]); }, 16);
        }
        begin = end;
    }
}

std::vector<std::size_t> GuideHierarchy::subtrees(const std::vector<GuideId>& roots, bool includeRoots) const {
    // Parent -> children adjacency, then everything reachable from `roots`.
    std::unordered_map<GuideId, std::vector<std::size_t>> children;
    for (std::size_t i = 0; i < guides_.size(); ++i) {
        if (guides_[i].isChild()) children[guides_[i].parent].push_back(i);
    }
    std::vector<char> isRoot(guides_.size(), 0);
    std::vector<std::size_t> stack;
    for (const GuideId id : roots) {
        auto it = index_.find(id);
        if (it == index_.end()) continue;
        isRoot[it->second] = 1;
        stack.push_back(it->second);
    }
    std::vector<std::size_t> found;
    std::vector<char> seen(guides_.size(), 0);
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        if (seen[i]) continue;
        seen[i] = 1;
        if (includeRoots || !isRoot[i]) found.push_back(i);
        auto it = children.find(guides_[i].id);
        if (it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
    }
    return depthOrder(found);
}

void GuideHierarchy::evaluateSubtrees(const ScalpMesh* scalp, const std::vector<GuideId>& changed) {
    invalidateHash();
    evaluateInOrder(subtrees(changed, true), scalp);
}

void GuideHierarchy::pose(GuideId id, std::vector<Vec3f> points) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g || points.size() < 2) return;
    // Carry the frame's up vector along with the root direction.
    if (g->points.size() >= 2) g->frameUp = rotateBetween(g->frameUp, rootTangent(g->points), rootTangent(points));
    g->points = std::move(points);
}

void GuideHierarchy::pose(GuideId id, std::vector<Vec3f> points, const Vec3f& frameUp) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g || points.size() < 2) return;
    g->frameUp = frameUp;
    g->points = std::move(points);
}

void GuideHierarchy::evaluateDescendants(const ScalpMesh* scalp, const std::vector<GuideId>& guides) {
    invalidateHash();
    evaluateInOrder(subtrees(guides, false), scalp);
}

// ---------------------------------------------------------------------------
// Hierarchy tools

namespace {

void makeFollower(HierarchyGuide& g, GuideId parent) {
    g.parent = parent;
    g.rootLocal.clear();
}

// The side of the symmetry plane a guide's root is on: 1 above `center`, -1 below, 0 on it.
int sideOf(const HierarchyGuide& g, int axis, float center) {
    const Vec3f& r = g.points.front();
    const float c = (axis == 0 ? r.x : axis == 1 ? r.y : r.z) - center;
    // On it within a hundred-thousandth of the guide's length and of where it is (its rounding):
    // the same at any scene scale.
    const float onPlane = 1e-5f * (arcLength(g.points) + std::max({std::abs(r.x), std::abs(r.y), std::abs(r.z)}));
    return std::abs(c) <= onPlane ? 0 : (c > 0.0f ? 1 : -1);
}

// A guide's points mirrored across the plane.
std::vector<Vec3f> mirroredPoints(const HierarchyGuide& g, int axis, float center) {
    std::vector<Vec3f> pts;
    pts.reserve(g.points.size());
    for (const Vec3f& p : g.points) pts.push_back(mirrorPoint(p, axis, center));
    return pts;
}

}  // namespace

void GuideHierarchy::reparent(const std::vector<GuideId>& ids, GuideId newParent) {
    invalidateHash();
    const HierarchyGuide* parent = guide(newParent);
    if (!parent) throw std::invalid_argument("reparent: unknown parent guide");
    for (const GuideId id : ids) {
        HierarchyGuide* g = mutableGuide(id);
        if (!g) throw std::invalid_argument("reparent: unknown guide");
        const GuideGroup* grp = group(g->group);
        if (!grp || grp->parent != parent->group) {
            throw std::invalid_argument("reparent: the new parent must be in the guide's parent group");
        }
    }
    for (const GuideId id : ids) {
        HierarchyGuide* g = mutableGuide(id);
        makeFollower(*g, newParent);
        if (!g->locked) bindOffsets(*g, *guide(newParent));
    }
}

void GuideHierarchy::promoteGroup(GroupId groupId) {
    invalidateHash();
    GuideGroup* grp = nullptr;
    for (GuideGroup& g : groups_) {
        if (g.id == groupId) grp = &g;
    }
    if (!grp) throw std::invalid_argument("promoteGroup: unknown group");
    if (grp->parent == kNoGroup) throw std::invalid_argument("promoteGroup: '" + grp->name + "' is already a root group");
    const GuideGroup* parentGroup = group(grp->parent);
    const GroupId grandparent = parentGroup ? parentGroup->parent : kNoGroup;
    for (const GuideId id : guidesInGroup(groupId)) {
        HierarchyGuide* g = mutableGuide(id);
        const HierarchyGuide* parent = guide(g->parent);
        const GuideId newParent = parent ? parent->parent : kNoGuide;
        if (newParent == kNoGuide) {
            // A primary now: bound to the scalp by its current shape at the next evaluate.
            g->parent = kNoGuide;
            g->locked = false;
            g->offsets.clear();
            g->rootLocal.clear();
        } else {
            makeFollower(*g, newParent);
            if (!g->locked) bindOffsets(*g, *guide(newParent));
        }
    }
    grp->parent = grandparent;
}

void GuideHierarchy::demoteGroup(GroupId groupId, GroupId newParent) {
    invalidateHash();
    GuideGroup* grp = nullptr;
    for (GuideGroup& g : groups_) {
        if (g.id == groupId) grp = &g;
    }
    const GuideGroup* target = group(newParent);
    if (!grp || !target) throw std::invalid_argument("demoteGroup: unknown group");
    if (newParent == groupId || target->parent != grp->parent) {
        throw std::invalid_argument("demoteGroup: the new parent group must be a sibling of '" + grp->name + "'");
    }
    const std::vector<GuideId> parents = guidesInGroup(newParent);
    if (parents.empty()) throw std::invalid_argument("demoteGroup: '" + target->name + "' has no guides to follow");
    for (const GuideId id : guidesInGroup(groupId)) {
        HierarchyGuide* g = mutableGuide(id);
        GuideId nearest = parents.front();
        float best = std::numeric_limits<float>::max();
        for (const GuideId p : parents) {
            const HierarchyGuide* pg = guide(p);
            if (pg->points.empty() || g->points.empty()) continue;
            const float d = (pg->points.front() - g->points.front()).length();
            if (d < best) {
                best = d;
                nearest = p;
            }
        }
        const bool wasPrimary = !g->isChild();
        makeFollower(*g, nearest);
        if (wasPrimary) g->locked = false;
        if (!g->locked) bindOffsets(*g, *guide(nearest));
    }
    grp->parent = newParent;
}

void GuideHierarchy::removeGuides(const std::vector<GuideId>& ids) {
    invalidateHash();
    std::set<GuideId> doomed;
    for (const std::size_t i : subtrees(ids, true)) doomed.insert(guides_[i].id);
    guides_.erase(std::remove_if(guides_.begin(), guides_.end(),
                                 [&](const HierarchyGuide& g) { return doomed.count(g.id) > 0; }),
                  guides_.end());
    rebuildIndex();
}

std::vector<GuideId> GuideHierarchy::mirrorGroup(GroupId groupId, int axis, float center, bool fromPositive) {
    invalidateHash();
    const GuideGroup* grp = group(groupId);
    if (!grp) throw std::invalid_argument("mirrorGroup: unknown group");
    if (grp->parent != kNoGroup) throw std::invalid_argument("mirrorGroup: mirror root groups (then regenerate children)");
    if (axis < 0 || axis > 2) throw std::invalid_argument("mirrorGroup: axis must be 0, 1 or 2");
    const int source = fromPositive ? 1 : -1;
    std::vector<GuideId> sources, doomed;
    for (const GuideId id : guidesInGroup(groupId)) {
        const HierarchyGuide* g = guide(id);
        if (g->points.empty()) continue;
        const int s = sideOf(*g, axis, center);
        if (s == source) sources.push_back(id);
        if (s == -source) doomed.push_back(id);
    }
    removeGuides(doomed);

    std::vector<GuideId> created;
    for (const GuideId id : sources) {
        const HierarchyGuide src = *guide(id);  // copy: addGuide grows guides_
        const GuideId copy = addGuide(groupId, mirroredPoints(src, axis, center), src.tube);
        HierarchyGuide* c = mutableGuide(copy);
        c->attributes = src.attributes;
        c->pointAttributes = src.pointAttributes;
        c->frameUp = mirrorVector(src.frameUp, axis);  // its frames, and its children, mirror too
        created.push_back(copy);
    }
    return created;
}

std::vector<GuideId> GuideHierarchy::mirrorAll(int axis, float center, bool fromPositive) {
    invalidateHash();
    if (axis < 0 || axis > 2) throw std::invalid_argument("mirrorAll: axis must be 0, 1 or 2");
    const int source = fromPositive ? 1 : -1;
    std::vector<GroupId> roots;
    for (const GuideGroup& g : groups_) {
        if (g.parent == kNoGroup) roots.push_back(g.id);
    }
    std::vector<GuideId> created;
    std::unordered_map<GuideId, std::vector<GuideId>> childrenById;
    // A guide and its descendants, mirrored, under `parentCopy` (none: a primary).
    std::function<void(GuideId, GuideId)> copyTree = [&](GuideId id, GuideId parentCopy) {
        const HierarchyGuide src = *guide(id);  // copy: adding guides grows guides_
        if (src.points.size() < 2) return;
        std::vector<Vec3f> pts = mirroredPoints(src, axis, center);
        const GuideId copy = parentCopy == kNoGuide ? addGuide(src.group, std::move(pts), src.tube)
                                                    : addChildGuide(src.group, parentCopy, std::move(pts), src.tube);
        HierarchyGuide* c = mutableGuide(copy);
        c->attributes = src.attributes;
        c->pointAttributes = src.pointAttributes;
        c->frameUp = mirrorVector(src.frameUp, axis);
        created.push_back(copy);
        if (src.locked) lock(copy);
        auto kids = childrenById.find(id);
        if (kids != childrenById.end()) {
            for (const GuideId child : kids->second) copyTree(child, copy);
        }
    };
    for (const GroupId gid : roots) {
        std::vector<GuideId> sources, doomed;
        for (const GuideId id : guidesInGroup(gid)) {
            const HierarchyGuide* g = guide(id);
            if (g->points.empty()) continue;
            const int s = sideOf(*g, axis, center);
            if (s == source) sources.push_back(id);
            if (s == -source) doomed.push_back(id);
        }
        removeGuides(doomed);
        childrenById.clear();  // the sources' subtrees, before any copy is added
        for (const HierarchyGuide& g : guides_) {
            if (g.isChild()) childrenById[g.parent].push_back(g.id);
        }
        for (const GuideId id : sources) copyTree(id, kNoGuide);
    }
    return created;
}

void GuideHierarchy::lock(GuideId id) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g || !g->isChild()) return;
    g->locked = true;
    g->offsets.clear();
    g->rootLocal.clear();  // binds to the scalp on the next evaluate
}

void GuideHierarchy::unlock(GuideId id) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g || !g->isChild() || !g->locked) return;
    const HierarchyGuide* parent = guide(g->parent);
    if (!parent) return;
    bindOffsets(*g, *parent);
    g->locked = false;
    g->rootLocal.clear();
}

void GuideHierarchy::transfer(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit) {
    transfer(ScalpMapping(from, to, fit));
}

void GuideHierarchy::transfer(const ScalpMapping& mapping) {
    invalidateHash();
    const ScalpMesh& from = mapping.from();
    const ScalpMesh& to = mapping.to();
    if (to.triangleCount() == 0) throw std::invalid_argument("transfer: the new scalp has no faces");
    evaluate(&from);
    // The scale each moved guide took (its subtree's tube radii and offsets follow it).
    std::unordered_map<GuideId, float> scales;
    for (HierarchyGuide& g : guides_) {
        if (g.followsParent() || g.points.empty()) continue;
        const bool rooted = g.root.face >= 0 && static_cast<std::size_t>(g.root.face) < from.triangleCount();
        const Vec3f oldRoot = g.points.front();
        if (mapping.byNormals()) {
            // closest and bounds: re-rooted at the closest point, turned by the least rotation
            // between the normals, scaled by the fit (as transfers always were).
            const ScalpFit& fit = mapping.fit();
            const float scale = mapping.options().keepLengths ? 1.0f : fit.scale;
            const Vec3f oldNormal = rooted ? from.normal(g.root) : from.closestPoint(oldRoot).normal;
            const SurfacePoint target = to.closestPoint(fit.apply(oldRoot));
            const Vec3f n0 = oldNormal.normalized();
            const Vec3f n1 = target.normal.normalized();
            for (Vec3f& p : g.points) p = target.position + rotateBetween((p - oldRoot) * scale, n0, n1);
            g.frameUp = rotateBetween(g.frameUp, n0, n1);
            g.root = target.root;
            scales[g.id] = scale;
        } else {
            // The shape carried by the mapping's local rotation and scale at the root.
            const MappedPoint m = rooted ? mapping.map(g.root) : mapping.mapPoint(oldRoot);
            if (m.target.root.face < 0) continue;
            const float scale = mapping.shapeScale(m);
            for (Vec3f& p : g.points) p = m.target.position + m.rotation * ((p - oldRoot) * scale);
            g.frameUp = m.rotation * g.frameUp;
            g.root = m.target.root;
            scales[g.id] = scale;
        }
        g.rootLocal.clear();  // bind on `to` below
    }
    // A follower takes the scale of the nearest ancestor that moved.
    auto scaleOf = [&](const HierarchyGuide& g) {
        const HierarchyGuide* at = &g;
        for (std::size_t steps = 0; at && steps <= guides_.size(); ++steps) {
            const auto it = scales.find(at->id);
            if (it != scales.end()) return it->second;
            at = guide(at->parent);
        }
        return mapping.byNormals() && !mapping.options().keepLengths ? mapping.fit().scale : 1.0f;
    };
    for (HierarchyGuide& g : guides_) {
        const float scale = scaleOf(g);
        g.tube.scaleWidth(scale);
        // Child offsets are in parent tube sections across (scaled above) and scene units along.
        if (g.followsParent()) {
            for (Vec3f& o : g.offsets) o.z *= scale;
        }
    }
    evaluate(&to);
}

void GuideHierarchy::setPoints(GuideId id, std::vector<Vec3f> points) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g) throw std::invalid_argument("setPoints: unknown guide");
    g->points = std::move(points);
    g->rootLocal.clear();  // re-bind to the scalp on the next evaluate
    if (g->followsParent()) {
        if (const HierarchyGuide* parent = guide(g->parent)) bindOffsets(*g, *parent);
    }
    fitLayers(*g);
}

void GuideHierarchy::setTube(GuideId id, TubeProfile tube) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g) return;
    // A primary given a shape (another shape, or its first: round to shaped) lays its frame across
    // the scalp at its next evaluate, so the new shape lies flat on the head at twist 0; not one
    // with children (they would turn round it).
    const bool newShape = tube.shaped() && (!g->tube.shaped() || !tube.sameShape(g->tube));
    if (newShape && !g->isChild()) {
        const bool parent = std::any_of(guides_.begin(), guides_.end(), [&](const HierarchyGuide& c) { return c.parent == id; });
        if (!parent) g->layFrame = true;
    }
    g->tube = std::move(tube);
    adoptTubeShape(g->tube);
}

void GuideHierarchy::adoptTubeShape(TubeProfile& tube) {
    if (!tube.custom) return;
    invalidateHash();
    const auto it = tubeShapes_.find(tube.custom->name);
    if (it == tubeShapes_.end()) {
        tubeShapes_.emplace(tube.custom->name, tube.custom);
    } else {
        tube.custom = it->second;
    }
}

CustomTubeShapePtr GuideHierarchy::tubeShape(const std::string& name) const {
    const auto it = tubeShapes_.find(name);
    return it == tubeShapes_.end() ? nullptr : it->second;
}

CustomTubeShapePtr GuideHierarchy::setTubeShape(const std::string& name, const std::vector<SectionPoint>& points, bool smooth) {
    CustomTubeShapePtr shape = makeCustomTubeShape(name, points, smooth);  // throws before anything changes
    invalidateHash();
    tubeShapes_[name] = shape;
    for (HierarchyGuide& g : guides_) {
        if (g.tube.custom && g.tube.custom->name == name) g.tube.custom = shape;
    }
    return shape;
}

void GuideHierarchy::renameTubeShape(const std::string& from, const std::string& to) {
    const auto it = tubeShapes_.find(from);
    if (it == tubeShapes_.end()) throw std::invalid_argument("no tube shape named '" + from + "'");
    if (from == to) return;
    checkTubeShapeName(to);
    if (tubeShapes_.count(to)) throw std::invalid_argument("a tube shape named '" + to + "' exists");
    invalidateHash();
    auto renamed = std::make_shared<CustomTubeShape>(*it->second);
    renamed->name = to;
    tubeShapes_.erase(it);
    tubeShapes_[to] = renamed;
    for (HierarchyGuide& g : guides_) {
        if (g.tube.custom && g.tube.custom->name == from) g.tube.custom = renamed;
    }
}

void GuideHierarchy::setLengthPaths(LengthPathSet paths) {
    lengthPaths_ = std::move(paths);
    invalidateHash();
}

std::size_t GuideHierarchy::removeTubeShape(const std::string& name) {
    if (!tubeShapes_.count(name)) throw std::invalid_argument("no tube shape named '" + name + "'");
    invalidateHash();
    tubeShapes_.erase(name);
    std::size_t reverted = 0;
    for (HierarchyGuide& g : guides_) {
        if (g.tube.custom && g.tube.custom->name == name) {
            g.tube.custom = nullptr;
            g.tube.shape = TubeShape::Round;
            ++reverted;
        }
    }
    return reverted;
}

std::size_t GuideHierarchy::tubeShapeUsers(const std::string& name) const {
    std::size_t users = 0;
    for (const HierarchyGuide& g : guides_) users += g.tube.custom && g.tube.custom->name == name ? 1 : 0;
    return users;
}

void GuideHierarchy::setAttribute(GuideId id, const std::string& name, float value) {
    invalidateHash();
    if (HierarchyGuide* g = mutableGuide(id)) g->attributes[name] = value;
}

void GuideHierarchy::setPointAttribute(GuideId id, const std::string& name, std::vector<float> values) {
    invalidateHash();
    HierarchyGuide* g = mutableGuide(id);
    if (!g) return;
    if (values.empty()) {
        g->pointAttributes.erase(name);
        return;
    }
    g->pointAttributes[name] = resampleLayer(values, g->points.size());
}

// ---------------------------------------------------------------------------
// Output

std::map<std::string, float> GuideHierarchy::effectiveAttributes(const HierarchyGuide& g) const {
    // Ancestors first, so a guide's own values override what it inherits.
    std::vector<const HierarchyGuide*> chain{&g};
    for (const HierarchyGuide* p = guide(g.parent); p && chain.size() <= guides_.size(); p = guide(p->parent)) {
        chain.push_back(p);
    }
    std::map<std::string, float> out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& [name, value] : (*it)->attributes) out[name] = value;
    }
    return out;
}

bool GuideHierarchy::effectiveAttribute(const HierarchyGuide& g, const std::string& name, float* value) const {
    std::size_t steps = 0;
    for (const HierarchyGuide* p = &g; p && steps <= guides_.size(); p = guide(p->parent), ++steps) {
        auto it = p->attributes.find(name);
        if (it != p->attributes.end()) {
            *value = it->second;
            return true;
        }
    }
    return false;
}

std::map<std::string, std::vector<float>> GuideHierarchy::effectivePointAttributes(const HierarchyGuide& g) const {
    // Ancestors first, so a guide's own layers override what it inherits; each
    // inherited layer is resampled along the guide to its own point count.
    std::vector<const HierarchyGuide*> chain{&g};
    for (const HierarchyGuide* p = guide(g.parent); p && chain.size() <= guides_.size(); p = guide(p->parent)) {
        chain.push_back(p);
    }
    std::map<std::string, std::vector<float>> out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& [name, values] : (*it)->pointAttributes) out[name] = resampleLayer(values, g.points.size());
    }
    return out;
}

bool GuideHierarchy::effectivePointAttribute(const HierarchyGuide& g, const std::string& name,
                                             std::vector<float>* values) const {
    std::size_t steps = 0;
    for (const HierarchyGuide* p = &g; p && steps <= guides_.size(); p = guide(p->parent), ++steps) {
        auto it = p->pointAttributes.find(name);
        if (it != p->pointAttributes.end()) {
            *values = resampleLayer(it->second, g.points.size());
            return true;
        }
    }
    return false;
}

bool GuideHierarchy::hasPointAttributes() const {
    for (const HierarchyGuide& g : guides_) {
        if (!g.pointAttributes.empty()) return true;
    }
    return false;
}

std::vector<std::string> GuideHierarchy::pointAttributeNames() const {
    std::set<std::string> names;
    for (const HierarchyGuide& g : guides_) {
        for (const auto& [name, values] : g.pointAttributes) names.insert(name);
    }
    return {names.begin(), names.end()};
}

std::vector<std::string> GuideHierarchy::attributeNames() const {
    std::set<std::string> names;
    for (const HierarchyGuide& g : guides_) {
        for (const auto& [name, value] : g.attributes) names.insert(name);
        for (const auto& [name, values] : g.pointAttributes) names.insert(name);
    }
    return {names.begin(), names.end()};
}

void GuideHierarchy::removeAttribute(const std::string& name) {
    invalidateHash();
    for (HierarchyGuide& g : guides_) {
        g.attributes.erase(name);
        g.pointAttributes.erase(name);
    }
}

void GuideHierarchy::renameAttribute(const std::string& from, const std::string& to) {
    invalidateHash();
    if (from == to) return;
    const auto names = attributeNames();
    if (std::find(names.begin(), names.end(), to) != names.end()) {
        throw std::invalid_argument("a guide attribute named '" + to + "' already exists");
    }
    for (HierarchyGuide& g : guides_) {
        auto it = g.attributes.find(from);
        if (it != g.attributes.end()) {
            const float value = it->second;
            g.attributes.erase(it);
            g.attributes[to] = value;
        }
        auto pt = g.pointAttributes.find(from);
        if (pt != g.pointAttributes.end()) {
            std::vector<float> values = std::move(pt->second);
            g.pointAttributes.erase(pt);
            g.pointAttributes[to] = std::move(values);
        }
    }
}

bool GuideHierarchy::hasAttributes() const {
    for (const HierarchyGuide& g : guides_) {
        if (!g.attributes.empty()) return true;
    }
    return false;
}

GuideSet GuideHierarchy::flattenGroup(GroupId groupId) const {
    GuideSet set;
    const bool attributes = hasAttributes();
    const bool layers = hasPointAttributes();
    const int depth = groupDepth(groupId);
    for (const HierarchyGuide& g : guides_) {
        if (g.group != groupId || g.points.size() < 2) continue;
        Guide& out = set.add(g.id, g.points);
        out.root = g.root;
        out.hasTube = true;
        out.tube = g.tube;
        out.tube.scaleWidth(scale_);
        out.frameUp = g.frameUp;
        out.depth = depth;
        if (attributes) out.attributes = effectiveAttributes(g);
        if (layers) out.pointAttributes = effectivePointAttributes(g);
    }
    return set;
}

GuideSet GuideHierarchy::flatten(int level) const {
    GuideSet set;
    const bool attributes = hasAttributes();
    const bool layers = hasPointAttributes();
    std::unordered_map<GroupId, int> depths;
    for (const GuideGroup& grp : groups_) depths[grp.id] = groupDepth(grp.id);
    const std::set<GroupId> deepest = level < 0 ? deepestGroups() : std::set<GroupId>{};
    for (const HierarchyGuide& g : guides_) {
        if ((level < 0 ? !deepest.count(g.group) : depths[g.group] != level) || g.points.size() < 2) continue;
        Guide& out = set.add(g.id, g.points);
        out.root = g.root;
        out.hasTube = true;
        out.tube = g.tube;
        out.tube.scaleWidth(scale_);
        out.frameUp = g.frameUp;
        out.depth = depths[g.group];
        if (attributes) out.attributes = effectiveAttributes(g);
        if (layers) out.pointAttributes = effectivePointAttributes(g);
    }
    return set;
}

std::uint64_t GuideHierarchy::contentHash() const {
    if (hashMemo_.valid.load(std::memory_order_acquire)) return hashMemo_.value.load(std::memory_order_relaxed);
    const std::uint64_t h = computeContentHash();
    hashMemo_.value.store(h, std::memory_order_relaxed);
    hashMemo_.valid.store(true, std::memory_order_release);
    return h;
}

std::uint64_t GuideHierarchy::computeContentHash() const {
    std::uint64_t h = hashCombine(hash64(groups_.size()), boundTopology_);
    if (scale_ != 1.0f) h = hashFloat(h, scale_);  // (a hierarchy at 1 hashes as it always has)
    for (const GuideGroup& g : groups_) {
        h = hashCombine(hashCombine(h, g.id), g.parent);
        h = hashString(h, g.name);
        if (g.segments > 0) h = hashCombine(h, static_cast<std::uint64_t>(g.segments));  // (none: hashes as it always has)
    }
    if (!tubeShapes_.empty()) {  // (a hierarchy without custom shapes hashes as it always has)
        h = hashCombine(h, tubeShapes_.size());
        for (const auto& [name, shape] : tubeShapes_) h = hashCombine(hashString(h, name), shape->hash);
    }
    if (!lengthPaths_.paths.empty()) h = hashString(hashCombine(h, 0x1E6Bu), lengthPaths_.toJson());  // (none: as it always has)
    h = hashCombine(h, guides_.size());
    for (const HierarchyGuide& g : guides_) {
        h = hashCombine(hashCombine(hashCombine(h, g.id), g.group), g.parent);
        h = hashCombine(h, g.locked ? 1 : 0);
        if (g.ownSegments) h = hashCombine(h, 0x05E6u);  // (none: as it always has)
        h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(g.root.face)));
        h = hashFloat(hashFloat(h, g.root.u), g.root.v);
        h = hashVec(h, g.frameUp);
        h = hashTube(h, g.tube);
        for (const auto& [k, v] : g.attributes) h = hashFloat(hashString(h, k), v);
        for (const auto& [k, values] : g.pointAttributes) {
            h = hashCombine(hashString(h, k), values.size());
            for (const float v : values) h = hashFloat(h, v);
        }
        h = hashCombine(h, g.points.size());
        for (const Vec3f& p : g.points) h = hashVec(h, p);
        h = hashCombine(h, g.offsets.size());
        for (const Vec3f& o : g.offsets) h = hashVec(h, o);
        if (g.pointCount > 0) h = hashCombine(h, static_cast<std::uint64_t>(g.pointCount) + 0x9C0u);  // (none: as it always has)
        h = hashCombine(h, g.rootLocal.size());
        for (const Vec3f& l : g.rootLocal) h = hashVec(h, l);
        h = hashVec(h, g.frameUpLocal);
    }
    return h;
}

// ---------------------------------------------------------------------------
// JSON

namespace {

using json = nlohmann::json;

json vecsToJson(const std::vector<Vec3f>& v) {
    json a = json::array();
    for (const Vec3f& p : v) {
        a.push_back(p.x);
        a.push_back(p.y);
        a.push_back(p.z);
    }
    return a;
}

std::vector<Vec3f> vecsFromJson(const json& a) {
    if (a.size() % 3 != 0) throw std::runtime_error("point array length is not a multiple of 3");
    std::vector<Vec3f> v(a.size() / 3);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = {a[3 * i].get<float>(), a[3 * i + 1].get<float>(), a[3 * i + 2].get<float>()};
    }
    return v;
}

json guideToJson(const HierarchyGuide& g) {
    json j = {
        {"id", g.id},
        {"group", g.group},
        {"root", {g.root.face, g.root.u, g.root.v}},
        {"frameUp", {g.frameUp.x, g.frameUp.y, g.frameUp.z}},
        {"tube", {{"shape", g.tube.shapeName()}, {"width", g.tube.width}, {"aspect", g.tube.aspect}, {"twist", g.tube.twist}}},
    };
    if (g.parent != kNoGuide) j["parent"] = g.parent;
    if (g.locked) j["locked"] = true;
    if (g.ownSegments) j["ownSegments"] = true;
    if (!g.attributes.empty()) j["attributes"] = g.attributes;
    if (!g.pointAttributes.empty()) j["pointAttributes"] = g.pointAttributes;
    if (g.followsParent()) {
        j["offsets"] = vecsToJson(g.offsets);
        if (g.pointCount > 0) j["pointCount"] = g.pointCount;
    } else {
        j["points"] = vecsToJson(g.points);
        if (!g.rootLocal.empty()) {
            j["rootLocal"] = vecsToJson(g.rootLocal);
            j["frameUpLocal"] = {g.frameUpLocal.x, g.frameUpLocal.y, g.frameUpLocal.z};
        }
    }
    return j;
}

// A guide from its JSON. Its tube's shape: a built-in's name, or one of `library`'s; another
// (a library shape that is missing) loads round, its name in `missing`.
HierarchyGuide guideFromJson(const json& j, const TubeShapeLibrary& library, std::string* missing) {
    HierarchyGuide g;
    g.id = j.at("id").get<GuideId>();
    g.group = j.at("group").get<GroupId>();
    g.parent = j.value("parent", kNoGuide);
    g.locked = j.value("locked", false);
    g.ownSegments = j.value("ownSegments", false);
    const json& r = j.at("root");
    g.root = {r.at(0).get<std::int32_t>(), r.at(1).get<float>(), r.at(2).get<float>()};
    const json& up = j.at("frameUp");
    g.frameUp = {up.at(0).get<float>(), up.at(1).get<float>(), up.at(2).get<float>()};
    // The tube's sections (tube.h); what is missing keeps its default (a groom saved before
    // sections: its tubes' old "radius" is not read, by decision: no format migrations).
    if (j.contains("tube")) {
        const json& tube = j.at("tube");
        // (A crescent, a shape there was for a while, reads as round.)
        if (tube.contains("shape")) {
            const std::string shape = tube.at("shape").get<std::string>();
            if (!parseTubeShape(shape, &g.tube.shape) && shape != "crescent") {
                const auto it = library.find(shape);
                if (it != library.end()) {
                    g.tube.custom = it->second;
                } else if (missing) {
                    *missing = shape;
                }
            }
        }
        auto list = [&](const char* key, std::vector<float>& into) {
            if (!tube.contains(key) || !tube.at(key).is_array()) return;  // (a groom saved before sections: a number)
            std::vector<float> values = tube.at(key).get<std::vector<float>>();
            if (!values.empty()) into = std::move(values);
        };
        list("width", g.tube.width);
        list("aspect", g.tube.aspect);
        list("twist", g.tube.twist);
    }
    if (j.contains("attributes")) g.attributes = j.at("attributes").get<std::map<std::string, float>>();
    if (j.contains("pointAttributes")) {
        g.pointAttributes = j.at("pointAttributes").get<std::map<std::string, std::vector<float>>>();
    }
    if (j.contains("points")) g.points = vecsFromJson(j.at("points"));
    if (j.contains("offsets")) g.offsets = vecsFromJson(j.at("offsets"));
    g.pointCount = std::min(j.value("pointCount", std::uint32_t{0}), std::uint32_t{257});
    if (j.contains("rootLocal")) g.rootLocal = vecsFromJson(j.at("rootLocal"));
    if (j.contains("frameUpLocal")) {
        const json& ul = j.at("frameUpLocal");
        g.frameUpLocal = {ul.at(0).get<float>(), ul.at(1).get<float>(), ul.at(2).get<float>()};
    }
    return g;
}

// Where a guide data text's guides are: the byte ranges of the elements of the root object's
// "guides" array, and the array's own range ('[' to past ']'). False when the text is not shaped
// as GuideHierarchy::toJson writes it (another "guides" key, nothing found, unbalanced): the
// caller parses it whole. A scan of the bytes, strings (and their escapes) skipped.
bool findGuideElements(const std::string& text, std::vector<std::pair<std::size_t, std::size_t>>& elements,
                       std::size_t& arrayBegin, std::size_t& arrayEnd) {
    elements.clear();
    arrayBegin = arrayEnd = std::string::npos;
    int depth = 0;
    bool inGuides = false, found = false;
    std::size_t elementBegin = std::string::npos;
    auto skipSpace = [&](std::size_t i) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
        return i;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            const std::size_t open = i;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\') ++i;
            }
            if (i >= text.size()) return false;
            // A key of the root object: "guides" followed by its array.
            if (depth == 1 && !inGuides && i - open - 1 == 6 && text.compare(open + 1, 6, "guides") == 0) {
                std::size_t k = skipSpace(i + 1);
                if (k >= text.size() || text[k] != ':') continue;  // a value, not a key
                if (found) return false;                            // twice: the last would win
                k = skipSpace(k + 1);
                if (k >= text.size() || text[k] != '[') return false;
                found = inGuides = true;
                arrayBegin = k;
                depth = 2;
                i = k;
                const std::size_t first = skipSpace(k + 1);
                if (first < text.size() && text[first] != ']') elementBegin = first;
            }
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (inGuides && depth == 2) {  // the guides array closes
                if (elementBegin != std::string::npos) elements.emplace_back(elementBegin, i);
                elementBegin = std::string::npos;
                inGuides = false;
                arrayEnd = i + 1;
            }
            if (--depth < 0) return false;
        } else if (c == ',' && inGuides && depth == 2) {
            if (elementBegin == std::string::npos) return false;
            elements.emplace_back(elementBegin, i);
            elementBegin = skipSpace(i + 1);
        }
    }
    return found && depth == 0 && arrayEnd != std::string::npos;
}

}  // namespace

std::string GuideHierarchy::toJson() const {
    json groups = json::array();
    for (const GuideGroup& g : groups_) {
        json group = {{"id", g.id}, {"name", g.name}, {"parent", g.parent}};
        if (g.segments > 0) group["segments"] = g.segments;
        groups.push_back(std::move(group));
    }
    // Each guide written on its own, side by side (the text of a big groom's guides took tens of
    // ms on one thread), then joined as one document's dump would join them: the root's keys in
    // their sorted order (nlohmann's objects keep them sorted), no spaces. The same bytes.
    std::vector<std::string> guides(guides_.size());
    parallelFor(0, guides_.size(), [&](std::size_t i) { guides[i] = guideToJson(guides_[i]).dump(); }, 16);
    std::size_t size = 64;
    for (const std::string& g : guides) size += g.size() + 1;
    std::string out;
    out.reserve(size);
    out += "{\"groups\":";
    out += groups.dump();
    out += ",\"guides\":[";
    for (std::size_t i = 0; i < guides.size(); ++i) {
        if (i) out += ',';
        out += guides[i];
    }
    out += ']';
    if (!lengthPaths_.paths.empty()) out += ",\"lengthPaths\":" + lengthPaths_.toJson();
    if (boundTopology_ != 0) out += ",\"scalpTopology\":" + json(boundTopology_).dump();
    if (!tubeShapes_.empty()) {
        // The shape library: each shape's name, its control points (u, v, u, v, ...) and smooth.
        json shapes = json::array();
        for (const auto& [name, shape] : tubeShapes_) {
            json points = json::array();
            for (const SectionPoint& p : shape->points) {
                points.push_back(p.u);
                points.push_back(p.v);
            }
            shapes.push_back({{"name", name}, {"points", std::move(points)}, {"smooth", shape->smooth}});
        }
        out += ",\"tubeShapes\":" + shapes.dump();
    }
    out += ",\"version\":" + json(currentSchemaVersion(DocumentKind::GuideData)).dump() + "}";
    return out;
}

GuideHierarchy GuideHierarchy::fromJson(const std::string& text) {
    GuideHierarchy h;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return h;
    try {
        // The guides parsed side by side, each from its own bytes, the rest of the document
        // with an empty guides array; anything else (not shaped so, a guide or the rest that does
        // not parse) is parsed whole, which gives the same hierarchy or the same error.
        std::vector<std::pair<std::size_t, std::size_t>> elements;
        std::size_t arrayBegin = 0, arrayEnd = 0;
        json root;
        std::vector<HierarchyGuide> guides;
        std::vector<std::string> missing;  // per guide: the library shape it lacks
        // The shape library, before the guides that take its shapes.
        auto readLibrary = [&h](const json& r) {
            h.tubeShapes_.clear();
            if (!r.is_object() || !r.contains("tubeShapes")) return;
            for (const json& s : r.at("tubeShapes")) {
                const std::string name = s.at("name").get<std::string>();
                const std::vector<float> flat = s.at("points").get<std::vector<float>>();
                std::vector<SectionPoint> points(flat.size() / 2);
                for (std::size_t i = 0; i < points.size(); ++i) points[i] = {flat[2 * i], flat[2 * i + 1]};
                try {
                    h.tubeShapes_[name] = makeCustomTubeShape(name, points, s.value("smooth", true));
                } catch (const std::invalid_argument& e) {
                    h.loadWarnings_.push_back(std::string("tube shape library: ") + e.what() + " (left out)");
                }
            }
        };
        bool split = findGuideElements(text, elements, arrayBegin, arrayEnd);
        if (split) {
            try {
                root = json::parse(text.substr(0, arrayBegin) + "[]" + text.substr(arrayEnd));
                // Another key that reads "guides" (escaped) with guides of its own: parsed whole.
                const auto other = root.is_object() ? root.find("guides") : root.end();
                if (other == root.end() || !other->is_array() || !other->empty()) throw std::runtime_error("not split");
                readLibrary(root);
                guides.resize(elements.size());
                missing.assign(elements.size(), std::string());
                parallelFor(0, elements.size(), [&](std::size_t i) {
                    const auto& [begin, end] = elements[i];
                    guides[i] = guideFromJson(json::parse(text.begin() + static_cast<std::ptrdiff_t>(begin),
                                                          text.begin() + static_cast<std::ptrdiff_t>(end)),
                                              h.tubeShapes_, &missing[i]);
                }, 4);
            } catch (const std::exception&) {
                split = false;
                h.loadWarnings_.clear();
            }
        }
        if (!split) {
            root = json::parse(text);
            guides.clear();
            missing.clear();
            readLibrary(root);
        }
        if (root.is_object() && root.empty()) return h;
        detail::checkVersion(DocumentKind::GuideData, root);  // refuses guide data from a newer GroomLab
        h.boundTopology_ = root.value("scalpTopology", std::uint64_t{0});
        if (root.contains("lengthPaths")) {
            try {
                h.lengthPaths_ = LengthPathSet::fromJson(root.at("lengthPaths").dump());
            } catch (const std::invalid_argument& e) {
                h.loadWarnings_.push_back(std::string(e.what()) + " (left out)");
            }
        }

        for (const json& g : root.value("groups", json::array())) {
            GuideGroup group{g.at("id").get<GroupId>(), g.at("name").get<std::string>(), g.value("parent", kNoGroup)};
            group.segments = std::clamp(g.value("segments", 0), 0, 256);
            h.nextGroupId_ = std::max(h.nextGroupId_, group.id + 1);
            h.groups_.push_back(std::move(group));
        }
        if (split) {
            h.guides_ = std::move(guides);
        } else {
            for (const json& j : root.value("guides", json::array())) {
                missing.emplace_back();
                h.guides_.push_back(guideFromJson(j, h.tubeShapes_, &missing.back()));
            }
        }
        // Guides whose library shape is missing load round: said once per shape.
        std::map<std::string, std::size_t> lacking;
        for (const std::string& name : missing) {
            if (!name.empty()) ++lacking[name];
        }
        for (const auto& [name, count] : lacking) {
            h.loadWarnings_.push_back("tube shape '" + name + "' is not in the groom's shape library: " + std::to_string(count) +
                                      (count == 1 ? " guide" : " guides") + " taking it loaded round");
        }
        h.rebuildIndex();
        for (const HierarchyGuide& g : h.guides_) {
            if (g.id < (GuideId{1} << 32)) h.nextGuideId_ = std::max(h.nextGuideId_, g.id + 1);
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid guide data: ") + e.what());
    }
    h.evaluate(nullptr);  // followers get their points back from offsets
    return h;
}

}  // namespace groomlab
