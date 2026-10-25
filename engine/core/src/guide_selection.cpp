// SPDX-License-Identifier: Apache-2.0
#include "groomlab/guide_selection.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

#include "groomlab/brush.h"
#include "groomlab/point_index.h"

namespace groomlab {

namespace {

bool insidePolygon(const std::array<float, 2>& p, const std::vector<std::array<float, 2>>& poly) {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) {
            inside = !inside;
        }
    }
    return inside;
}

Vec3f rootOf(const HierarchyGuide& g) { return g.points.empty() ? Vec3f{} : g.points.front(); }

}  // namespace

std::vector<GuideId> guidesAt(const GuideHierarchy& h, int level, const std::string& group) {
    std::vector<GuideId> ids;
    const GroupId gid = group.empty() ? kNoGroup : h.findGroup(group);
    if (!group.empty() && gid == kNoGroup) return ids;
    for (const HierarchyGuide& g : h.guides()) {
        if (gid != kNoGroup ? g.group == gid : (level < 0 || h.groupDepth(g.group) == level)) ids.push_back(g.id);
    }
    return ids;
}

std::vector<GuideId> guidesAt(const GuideHierarchy& h, int level, const std::string& group,
                              const std::set<std::string>& hiddenGroups) {
    std::vector<GuideId> ids = guidesAt(h, level, group);
    if (hiddenGroups.empty()) return ids;
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](GuideId id) {
                                 const HierarchyGuide* g = h.guide(id);
                                 const GuideGroup* grp = g ? h.group(g->group) : nullptr;
                                 return grp && hiddenGroups.count(grp->name);
                             }),
              ids.end());
    return ids;
}

std::vector<GuideId> guidesInLasso(const GuideHierarchy& h, const std::vector<GuideId>& candidates,
                                   const ViewProjector& project, const std::vector<std::array<float, 2>>& polygon,
                                   bool rootsOnly) {
    std::vector<GuideId> out;
    if (polygon.size() < 3) return out;
    for (const GuideId id : candidates) {
        const HierarchyGuide* g = h.guide(id);
        if (!g || g->points.empty()) continue;
        const std::size_t count = rootsOnly ? 1 : g->points.size();
        for (std::size_t i = 0; i < count; ++i) {
            std::array<float, 2> v{};
            if (project(g->points[i], &v) && insidePolygon(v, polygon)) {
                out.push_back(id);
                break;
            }
        }
    }
    return out;
}

ViewProjector pixelProjector(const ViewProjection& view) {
    return [view](const Vec3f& p, std::array<float, 2>* v) {
        int x = 0, y = 0;
        if (!view.projectPixel(p, x, y)) return false;
        *v = {static_cast<float>(x), static_cast<float>(y)};
        return true;
    };
}

std::shared_ptr<const ScalpVisibility> scalpVisibility(const std::shared_ptr<const ScalpMesh>& scalp, const ToolView& view) {
    Vec3f eye, along;
    bool ortho = false;
    if (!scalp || scalp->triangleCount() == 0 || !view.camera(eye, along, ortho)) return nullptr;
    return std::make_shared<const ScalpVisibility>(scalp, eye, along, ortho);
}

ViewProjector visibleProjector(ViewProjector project, std::shared_ptr<const ScalpVisibility> occlusion) {
    if (!occlusion) return project;
    return [project = std::move(project), occlusion = std::move(occlusion)](const Vec3f& world, std::array<float, 2>* view) {
        return project(world, view) && !occlusion->hidden(world);  // (on screen first: the cheaper test)
    };
}

std::vector<GuideId> pickGuides(const GuideHierarchy& h, const std::vector<GuideId>& candidates,
                                const ViewProjector& project, const std::vector<std::array<float, 2>>& lasso,
                                bool rootsOnly, float clickPixels, const ViewProjection* bounds) {
    std::vector<GuideId> picked;
    if (lasso.empty()) return picked;
    float extent = 0.0f;
    for (const auto& p : lasso) extent = std::max(extent, std::abs(p[0] - lasso[0][0]) + std::abs(p[1] - lasso[0][1]));
    if (lasso.size() >= 3 && extent > 3.0f) {
        if (!bounds || rootsOnly) return guidesInLasso(h, candidates, project, lasso, rootsOnly);
        // Guides whose projected bounds miss the lasso's cannot have a point inside it (pixels
        // are truncated: 2 pixels of slack).
        float lx = lasso[0][0], ly = lasso[0][1], hx = lx, hy = ly;
        for (const auto& p : lasso) {
            lx = std::min(lx, p[0]);
            ly = std::min(ly, p[1]);
            hx = std::max(hx, p[0]);
            hy = std::max(hy, p[1]);
        }
        std::vector<GuideId> near;
        near.reserve(candidates.size());
        for (const GuideId id : candidates) {
            const HierarchyGuide* g = h.guide(id);
            if (!g || g->points.empty()) continue;
            Vec3f lo = g->points.front(), hi = lo;
            for (const Vec3f& p : g->points) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            if (bounds->projectBox(lo, hi, x0, y0, x1, y1) &&
                (x1 + 2.0f < lx || x0 - 2.0f > hx || y1 + 2.0f < ly || y0 - 2.0f > hy)) {
                continue;
            }
            near.push_back(id);
        }
        return guidesInLasso(h, near, project, lasso, rootsOnly);
    }
    // A click: the guide with a point nearest the cursor, within clickPixels.
    GuideId best = kNoGuide;
    float bestDistance = clickPixels;
    for (const GuideId id : candidates) {
        const HierarchyGuide* g = h.guide(id);
        if (!g) continue;
        for (const Vec3f& p : g->points) {
            std::array<float, 2> v{};
            if (!project(p, &v)) continue;
            const float d = pm::hypot(v[0] - lasso[0][0], v[1] - lasso[0][1]);
            if (d < bestDistance) {
                bestDistance = d;
                best = id;
            }
        }
    }
    if (best != kNoGuide) picked.push_back(best);
    return picked;
}

bool nearestGuidePoint(const GuideHierarchy& h, const ToolView& view, float x, float y, float radius,
                       const Vec3f& origin, const Vec3f& direction, const ScalpVisibility* occlusion,
                       GuidePointHit* hit) {
    float best = radius * radius;
    float bestDepth = std::numeric_limits<float>::max();
    bool found = false;
    for (const HierarchyGuide& g : h.guides()) {
        for (const Vec3f& p : g.points) {
            float px = 0.0f, py = 0.0f;
            if (!view.project(p, px, py)) continue;
            const float dx = px - x, dy = py - y;
            const float d2 = dx * dx + dy * dy;
            const float depth = (p - origin).dot(direction);
            if (depth <= 0.0f) continue;  // behind the camera
            if (d2 < best + 1.0f && occlusion && occlusion->hidden(p)) continue;  // behind the head
            // Nearest on screen; among (nearly) equal ones, nearest the camera.
            if (d2 < best - 1.0f || (d2 <= best + 1.0f && depth < bestDepth)) {
                best = d2;
                bestDepth = depth;
                hit->guide = &g;
                hit->point = p;
                found = true;
            }
        }
    }
    return found;
}

SelectMode selectModeFromName(const std::string& name) {
    if (name == "replace") return SelectMode::Replace;
    if (name == "add") return SelectMode::Add;
    if (name == "toggle") return SelectMode::Toggle;
    if (name == "deselect") return SelectMode::Deselect;
    throw std::invalid_argument("a selection mode is replace, add, toggle or deselect, not " + name);
}

std::vector<GuideId> combineSelection(const std::vector<GuideId>& selected, const std::vector<GuideId>& picked, SelectMode mode) {
    std::vector<GuideId> result;
    switch (mode) {
        case SelectMode::Replace:
            result = picked;
            break;
        case SelectMode::Add: {
            result = selected;
            std::set<GuideId> in(result.begin(), result.end());
            for (const GuideId id : picked) {
                if (in.insert(id).second) result.push_back(id);
            }
            break;
        }
        case SelectMode::Deselect: {
            const std::set<GuideId> out(picked.begin(), picked.end());
            for (const GuideId id : selected) {
                if (!out.count(id)) result.push_back(id);
            }
            break;
        }
        case SelectMode::Toggle:
            result = selected;
            for (const GuideId id : picked) {
                const auto it = std::find(result.begin(), result.end(), id);
                if (it == result.end()) {
                    result.push_back(id);
                } else {
                    result.erase(it);
                }
            }
            break;
    }
    return result;
}

std::map<GuideId, float> softSelection(const GuideHierarchy& h, const std::vector<GuideId>& selected, float radius) {
    std::map<GuideId, float> weights;
    std::set<GroupId> groups;
    std::vector<Vec3f> roots;
    for (const GuideId id : selected) {
        const HierarchyGuide* g = h.guide(id);
        if (!g) continue;
        weights[id] = 1.0f;
        groups.insert(g->group);
        roots.push_back(rootOf(*g));
    }
    if (radius <= 0.0f || roots.empty()) return weights;
    const PointIndex index(roots);
    for (const HierarchyGuide& g : h.guides()) {
        if (weights.count(g.id) || !groups.count(g.group) || g.points.empty()) continue;
        const float w = brushFalloff((roots[index.nearest(rootOf(g))] - rootOf(g)).length(), radius);
        if (w > 0.0f) weights[g.id] = w;
    }
    return weights;
}

std::vector<GuideId> mirrorSelection(const GuideHierarchy& h, const std::vector<GuideId>& selected, int axis,
                                     float center, float tolerance) {
    // Each group's roots (guides with points, in the hierarchy's order), indexed when first needed.
    struct GroupRoots {
        std::vector<GuideId> ids;
        std::unique_ptr<PointIndex> index;
    };
    std::map<GroupId, GroupRoots> groups;
    auto rootsOf = [&](GroupId group) -> GroupRoots& {
        GroupRoots& r = groups[group];
        if (r.index) return r;
        std::vector<Vec3f> roots;
        for (const HierarchyGuide& other : h.guides()) {
            if (other.group != group || other.points.empty()) continue;
            r.ids.push_back(other.id);
            roots.push_back(rootOf(other));
        }
        r.index = std::make_unique<PointIndex>(std::move(roots));
        return r;
    };
    std::vector<GuideId> out;
    std::vector<std::size_t> indices;
    std::vector<float> distances;
    for (const GuideId id : selected) {
        const HierarchyGuide* g = h.guide(id);
        if (!g || g->points.empty()) continue;
        const Vec3f target = mirrorPoint(rootOf(*g), axis, center);
        const GroupRoots& r = rootsOf(g->group);
        const std::size_t n = r.ids.size();
        if (n == 0) continue;
        // The nearest roots, until one farther than the nearest (by length(), as a scan compares
        // them): the guide is the last in order among those at the least distance.
        std::size_t k = std::min<std::size_t>(4, n), found = 0;
        for (;;) {
            indices.resize(k);
            distances.resize(k);
            found = r.index->kNearest(target, k, indices.data(), distances.data());
            if (found < k || k == n || std::sqrt(distances[found - 1]) > std::sqrt(distances[0])) break;
            k = std::min(n, k * 4);
        }
        const float least = std::sqrt(distances[0]);
        if (!(least <= tolerance)) continue;
        std::size_t last = 0;
        bool any = false;
        for (std::size_t i = 0; i < found; ++i) {
            if (std::sqrt(distances[i]) != least) continue;
            if (!any || indices[i] > last) last = indices[i];
            any = true;
        }
        const GuideId best = r.ids[last];
        if (std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
    }
    return out;
}

}  // namespace groomlab
