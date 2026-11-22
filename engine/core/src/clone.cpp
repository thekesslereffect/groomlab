// SPDX-License-Identifier: Apache-2.0
// The Clone tool's capture and set-down (clone.h).

#include "groomlab/clone.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

#include "groomlab/brush.h"
#include "groomlab/frames.h"
#include "groomlab/math.h"
#include "groomlab/root_tubes.h"

namespace groomlab {

namespace {

Vec3f unitOr(const Vec3f& v, const Vec3f& fallback) {
    const float l = v.length();
    return l > 1e-12f ? v / l : fallback;
}

// The group of `h` named as `name` is in the clip's hierarchy; a missing root group is made, a
// missing child group is not (kNoGroup).
GroupId groupNamed(GuideHierarchy& h, const GuideHierarchy& source, GroupId sourceGroup) {
    const GuideGroup* g = source.group(sourceGroup);
    if (!g) return kNoGroup;
    const GroupId found = h.findGroup(g->name);
    if (found != kNoGroup) return found;
    return source.groupDepth(sourceGroup) == 0 ? h.addGroup(g->name) : kNoGroup;
}

// For a copy in child group `group` whose parent was not copied: the guide of the parent group
// whose root is nearest `root`.
GuideId nearestParent(const GuideHierarchy& h, GroupId group, const Vec3f& root) {
    const GuideGroup* g = h.group(group);
    if (!g) return kNoGuide;
    GuideId best = kNoGuide;
    float bestD2 = std::numeric_limits<float>::max();
    for (const HierarchyGuide& p : h.guides()) {
        if (p.group != g->parent || p.points.empty()) continue;
        const float d2 = (p.points.front() - root).dot(p.points.front() - root);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = p.id;
        }
    }
    return best;
}

// What a copy keeps of its source besides its shape and tube.
void copyGuideData(GuideHierarchy& h, GuideId to, const HierarchyGuide& from) {
    for (const auto& [name, value] : from.attributes) h.setAttribute(to, name, value);
    for (const auto& [name, values] : from.pointAttributes) h.setPointAttribute(to, name, values);
}

bool frozen(const GuideHierarchy& h, const HierarchyGuide& g, const std::string& freeze) {
    float v = 0.0f;
    return !freeze.empty() && h.effectiveAttribute(g, freeze, &v) && v >= 0.5f;
}

// How the clip lands at `at`: per captured guide, where its root goes and how its points (and its
// descendants') are carried there.
class Landing {
public:
    Landing(const ScalpMesh& scalp, const GuideClip& clip, const SurfacePoint& at, float angle, const StrokeSymmetry* mirror)
        : scalp_(scalp), clip_(clip), at_(at), angle_(angle), mirror_(mirror) {
        n0_ = clip.normal;
        n1_ = unitOr(at.normal, n0_);
    }
    // An offset from the clip's center carried to `at`: turned from its normal to `at`'s, then spun.
    Vec3f patch(const Vec3f& v) const { return rotateAxisAngle(rotateBetween(v, n0_, n1_), n1_, angle_); }
    // Guide k of the clip: false when its root lands off the scalp.
    bool begin(std::size_t k) {
        const HierarchyGuide* top = clip_.source->guide(clip_.guides[k]);
        if (!top || top->points.size() < 2) return false;
        r0_ = top->points.front();
        root_ = scalp_.closestPoint(at_.position + patch(r0_ - clip_.center));
        if (root_.root.face < 0) return false;
        nTop_ = patch(clip_.normals[k]);
        nRoot_ = unitOr(root_.normal, n1_);
        shift_ = {};
        if (mirror_) {  // the mirror image's root back on the scalp
            const Vec3f image = mirror_->point(root_.position);
            const SurfacePoint m = scalp_.closestPoint(image);
            if (m.root.face < 0) return false;
            shift_ = m.position - image;
        }
        return true;
    }
    const SurfacePoint& root() const { return root_; }
    // Points of the current guide or one under it, carried: about the guide's root, turned from its
    // captured normal (so patched) to the scalp's where it lands; mirrored when set.
    std::vector<Vec3f> shape(const std::vector<Vec3f>& points) const {
        std::vector<Vec3f> out;
        out.reserve(points.size());
        for (const Vec3f& p : points) {
            Vec3f q = root_.position + rotateBetween(patch(p - r0_), nTop_, nRoot_);
            if (mirror_) q = mirror_->point(q) + shift_;
            out.push_back(q);
        }
        return out;
    }

private:
    const ScalpMesh& scalp_;
    const GuideClip& clip_;
    SurfacePoint at_;
    float angle_;
    const StrokeSymmetry* mirror_;
    Vec3f n0_, n1_, r0_, nTop_, nRoot_, shift_;
    SurfacePoint root_;
};

}  // namespace

GuideClip captureGuides(const GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides,
                        const SurfacePoint& center, float radius) {
    GuideClip clip;
    clip.center = center.position;
    clip.normal = unitOr(center.normal, {0, 1, 0});
    clip.radius = radius;
    const std::set<GuideId> picked(guides.begin(), guides.end());
    for (const HierarchyGuide& g : h.guides()) {
        if (!picked.count(g.id) || g.points.size() < 2) continue;
        // A picked guide under another picked one comes along with it: not a patch guide of its own.
        bool under = false;
        for (GuideId p = g.parent; p != kNoGuide && !under;) {
            under = picked.count(p) > 0;
            const HierarchyGuide* pg = h.guide(p);
            p = pg ? pg->parent : kNoGuide;
        }
        if (under) continue;
        clip.guides.push_back(g.id);
        Vec3f n = g.rootNormal;
        if (n.length() < 1e-6f && scalp.triangleCount() > 0) n = scalp.closestPoint(g.points.front()).normal;
        clip.normals.push_back(unitOr(n, clip.normal));
    }
    if (!clip.guides.empty()) clip.source = std::make_shared<const GuideHierarchy>(h);
    return clip;
}

std::vector<GuideId> pasteGuides(GuideHierarchy& h, const ScalpMesh& scalp, const GuideClip& clip, const SurfacePoint& at,
                                 const CloneOptions& options, const StrokeSymmetry* mirror) {
    std::vector<GuideId> made;
    if (clip.empty() || at.root.face < 0 || scalp.triangleCount() == 0) return made;
    const GuideHierarchy& src = *clip.source;
    Landing landing(scalp, clip, at, options.angle, mirror);

    if (options.replace) {
        // The guides already there, of the groups the clip copies into, within its circle (round
        // `at`, or its mirror image), frozen ones kept.
        Vec3f center = at.position;
        if (mirror) {
            const SurfacePoint m = scalp.closestPoint(mirror->point(at.position));
            if (m.root.face < 0) return made;
            center = m.position;
        }
        std::set<GroupId> groups;
        for (const GuideId id : clip.guides) {
            const HierarchyGuide* g = src.guide(id);
            const GuideGroup* grp = g ? src.group(g->group) : nullptr;
            const GroupId mine = grp ? h.findGroup(grp->name) : kNoGroup;
            if (mine != kNoGroup) groups.insert(mine);
        }
        const float r2 = clip.radius * clip.radius;
        std::vector<GuideId> gone;
        for (const HierarchyGuide& g : h.guides()) {
            if (!groups.count(g.group) || g.points.empty() || frozen(h, g, options.freeze)) continue;
            if ((g.points.front() - center).dot(g.points.front() - center) <= r2) gone.push_back(g.id);
        }
        if (!gone.empty()) h.removeGuides(gone);
    }

    for (std::size_t k = 0; k < clip.guides.size(); ++k) {
        if (!landing.begin(k)) continue;
        const HierarchyGuide& top = *src.guide(clip.guides[k]);

        // A root tube that `h` still holds as captured: its own duplicate, with its tube hierarchy.
        if (!mirror && top.rootOutline.size() >= 3 && top.parent == kNoGuide) {
            const HierarchyGuide* live = h.guide(top.id);
            if (live && live->points == top.points) {
                try {
                    const Vec3f start = top.points.front();
                    made.push_back(duplicateRootTube(h, scalp, top.id, landing.root().position, &start));
                    continue;
                } catch (const std::invalid_argument&) {
                    // not a root tube after all: copied as any guide below
                }
            }
        }

        std::vector<Vec3f> pts = landing.shape(top.points);
        pts.front() = scalp.closestPoint(pts.front()).position;
        if (options.collide) pts = keepOffScalp(pts, scalp);
        const GroupId group = groupNamed(h, src, top.group);
        if (group == kNoGroup) continue;
        GuideId id = kNoGuide;
        if (h.groupDepth(group) == 0) {
            id = h.addGuide(group, pts, top.tube);
        } else {
            const GuideId parent = nearestParent(h, group, pts.front());
            if (parent == kNoGuide) continue;
            id = h.addChildGuide(group, parent, pts, top.tube);
        }
        copyGuideData(h, id, top);
        h.evaluateSubtrees(&scalp, {id});
        made.push_back(id);

        // Its descendants, level by level, each a child of its parent's copy, carried as it is.
        std::vector<std::pair<GuideId, GuideId>> level{{top.id, id}};  // (source, copy)
        while (!level.empty()) {
            std::vector<std::pair<GuideId, GuideId>> below;
            std::vector<GuideId> added;
            for (const auto& [from, to] : level) {
                for (const GuideId c : src.childrenOf(from)) {
                    const HierarchyGuide* child = src.guide(c);
                    const GuideGroup* sg = child ? src.group(child->group) : nullptr;
                    const HierarchyGuide* parentCopy = h.guide(to);
                    if (!sg || !parentCopy || child->points.size() < 2) continue;
                    GroupId childGroup = h.findGroup(sg->name);
                    // A level this groom lacks: made under the copy's group, as in the source.
                    if (childGroup == kNoGroup) childGroup = h.addGroup(sg->name, parentCopy->group);
                    if (h.group(childGroup)->parent != parentCopy->group) continue;  // a group of that name elsewhere
                    std::vector<Vec3f> cp = landing.shape(child->points);
                    if (options.collide) cp = keepOffScalp(cp, scalp);
                    const GuideId copy = h.addChildGuide(childGroup, to, cp, child->tube);
                    copyGuideData(h, copy, *child);
                    below.emplace_back(c, copy);
                    added.push_back(copy);
                }
            }
            if (!added.empty()) h.evaluateSubtrees(&scalp, added);
            level.swap(below);
        }
    }
    return made;
}

std::vector<std::vector<Vec3f>> clonePreview(const ScalpMesh& scalp, const GuideClip& clip, const SurfacePoint& at, float angle,
                                             const StrokeSymmetry* mirror, std::size_t maxLines) {
    std::vector<std::vector<Vec3f>> lines;
    if (clip.empty() || at.root.face < 0 || scalp.triangleCount() == 0) return lines;
    const GuideHierarchy& src = *clip.source;
    Landing landing(scalp, clip, at, angle, mirror);
    for (std::size_t k = 0; k < clip.guides.size() && lines.size() < maxLines; ++k) {
        if (!landing.begin(k)) continue;
        std::vector<GuideId> todo{clip.guides[k]};
        while (!todo.empty() && lines.size() < maxLines) {
            const GuideId id = todo.back();
            todo.pop_back();
            const HierarchyGuide* g = src.guide(id);
            if (!g) continue;
            if (g->points.size() >= 2) lines.push_back(landing.shape(g->points));
            for (const GuideId c : src.childrenOf(id)) todo.push_back(c);
        }
    }
    return lines;
}

}  // namespace groomlab
