// SPDX-License-Identifier: Apache-2.0
#include "groomlab/length_path_tool.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "groomlab/brush.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"

namespace groomlab {

namespace {

// The mesh keys are stored on: the scalp before it is refined (StrokeHost::paintScalp), as parts
// and flow are, so the keys stay put whatever the subdivision.
MeshPtr keyMesh(StrokeHost& host) {
    MeshPtr mesh = host.paintScalp();
    return mesh && mesh->triangleCount() > 0 ? mesh : host.scalp();
}

}  // namespace

LengthPathSet mirroredPaths(const LengthPathSet& paths, const BrushToolSettings& settings, const MeshPtr& mesh);

void LengthPathTool::load(StrokeHost& host) {
    if (loaded_ && edited_) return;  // the edits stand until Apply or Cancel
    // Not edited: the stored paths, read again each time (an undo, a script, another groom's).
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    LengthPathSet stored = h ? h->lengthPaths() : LengthPathSet{};
    if (loaded_ && stored.toJson() == paths_.toJson()) return;
    const bool keep = loaded_ && stored.paths.size() == paths_.paths.size();
    paths_ = std::move(stored);
    loaded_ = true;
    edited_ = false;
    if (keep) {  // the same paths (a length changed): the same selection
        select(host, activePath_, activeKey_);
        return;
    }
    activePath_ = paths_.paths.empty() ? -1 : static_cast<int>(paths_.paths.size()) - 1;
    activeKey_ = activePath_ >= 0 && !paths_.paths[activePath_].keys.empty()
                     ? static_cast<int>(paths_.paths[activePath_].keys.size()) - 1
                     : -1;
}

const LengthPathSet& LengthPathTool::paths(StrokeHost& host) {
    load(host);
    return paths_;
}

void LengthPathTool::reset(StrokeHost* host) {
    if (host && edited_) host->previewHierarchy(nullptr, false);
    loaded_ = edited_ = startNew_ = false;
    paths_ = {};
    activePath_ = activeKey_ = hoverPath_ = hoverKey_ = -1;
    drag_ = Drag::None;
}

std::vector<GuideId> LengthPathTool::targets(StrokeHost& host, const BrushToolSettings& settings, const GuideHierarchy& h) const {
    BrushSettings b = settings.brush;
    b.hiddenGroups = host.hiddenGroups();
    std::map<GuideId, float> selected;
    for (const GuideId id : host.selectedGuides()) selected[id] = 1.0f;
    b.mask = selected.empty() ? nullptr : &selected;
    return brushTargets(h, b);
}

// The paths mirrored as the brush's symmetry mirrors (a world plane, or the scalp's topology: each
// key at its partner spot on the scalp); none when symmetry is off.
LengthPathSet mirroredPaths(const LengthPathSet& paths, const BrushToolSettings& settings, const MeshPtr& mesh) {
    const StrokeSymmetry sym = StrokeSymmetry::of(settings.brush, mesh.get());
    if (!sym.on()) return {};
    return mirroredLengthPaths(paths, mesh.get(), [&sym](const Vec3f& p) { return sym.point(p); });
}

LengthPathSet LengthPathTool::acting(StrokeHost& host, const BrushToolSettings& settings) const {
    LengthPathSet both = paths_;
    const LengthPathSet mirrored = mirroredPaths(paths_, settings, keyMesh(host));
    both.paths.insert(both.paths.end(), mirrored.paths.begin(), mirrored.paths.end());
    return both;
}

std::shared_ptr<GuideHierarchy> LengthPathTool::result(StrokeHost& host, const BrushToolSettings& settings) {
    const std::shared_ptr<const GuideHierarchy> stored = host.authoredHierarchy();
    auto h = std::make_shared<GuideHierarchy>(stored ? *stored : GuideHierarchy{});
    const MeshPtr mesh = keyMesh(host);
    for (const LengthResult& r : applyLengthPaths(*h, targets(host, settings, *h), acting(host, settings), change, mesh.get())) {
        h->setPoints(r.id, r.points);
    }
    h->setLengthPaths(paths_);
    return h;
}

void LengthPathTool::preview(StrokeHost& host, const BrushToolSettings& settings) {
    edited_ = true;
    host.previewHierarchy(result(host, settings), false);
}

bool LengthPathTool::scalpPoint(StrokeHost& host, const ToolView& view, float x, float y, LengthKey& key) const {
    const MeshPtr shown = host.scalp();
    if (!shown || shown->triangleCount() == 0) return false;
    Vec3f origin, direction;
    SurfacePoint hit;
    if (!view.ray(x, y, origin, direction) || !shown->intersectRay(origin, direction, &hit)) return false;
    const MeshPtr mesh = keyMesh(host);
    key.point = hit.position;
    key.root = hit.root;
    if (mesh != shown) {
        const SurfacePoint onCage = mesh->closestPoint(hit.position);
        if (onCage.root.face >= 0) key.root = onCage.root;
    }
    return true;
}

bool LengthPathTool::keyAt(StrokeHost& host, const ToolView& view, float x, float y, int* path, int* key) {
    load(host);
    const MeshPtr mesh = keyMesh(host);
    float best = kPickPixels * kPickPixels;
    bool found = false;
    for (std::size_t p = 0; p < paths_.paths.size(); ++p) {
        for (std::size_t k = 0; k < paths_.paths[p].keys.size(); ++k) {
            float kx = 0.0f, ky = 0.0f;
            if (!view.project(lengthKeyPosition(paths_.paths[p].keys[k], mesh.get()), kx, ky)) continue;
            const float d2 = (kx - x) * (kx - x) + (ky - y) * (ky - y);
            if (d2 <= best) {
                best = d2;
                *path = static_cast<int>(p);
                *key = static_cast<int>(k);
                found = true;
            }
        }
    }
    return found;
}

bool LengthPathTool::segmentAt(StrokeHost& host, const ToolView& view, float x, float y, int* path, int* nextKey) {
    load(host);
    const MeshPtr mesh = keyMesh(host);
    float best = kPickPixels * kPickPixels;
    bool found = false;
    for (std::size_t p = 0; p < paths_.paths.size(); ++p) {
        const auto& keys = paths_.paths[p].keys;
        for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (!view.project(lengthKeyPosition(keys[k], mesh.get()), ax, ay) ||
                !view.project(lengthKeyPosition(keys[k + 1], mesh.get()), bx, by)) {
                continue;
            }
            const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
            const float t = len2 > 0.0f ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
            const float ex = ax + dx * t - x, ey = ay + dy * t - y;
            if (ex * ex + ey * ey <= best) {
                best = ex * ex + ey * ey;
                *path = static_cast<int>(p);
                *nextKey = static_cast<int>(k + 1);
                found = true;
            }
        }
    }
    return found;
}

void LengthPathTool::hover(StrokeHost& host, const ToolView& view, float x, float y) {
    if (!keyAt(host, view, x, y, &hoverPath_, &hoverKey_)) hoverPath_ = hoverKey_ = -1;
}

bool LengthPathTool::press(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y,
                           unsigned modifiers) {
    load(host);
    drag_ = Drag::None;
    int p = -1, k = -1;
    const bool onKey = keyAt(host, view, x, y, &p, &k);
    if (modifiers & kStrokeAdjust) {
        // The adjusting button: the key under the cursor's length, else the selected key's.
        if (onKey) {
            activePath_ = p;
            activeKey_ = k;
        }
        if (activePath_ < 0 || activeKey_ < 0) return false;
        drag_ = Drag::Length;
        startY_ = y;
        startLength_ = paths_.paths[activePath_].keys[activeKey_].length;
        return true;
    }
    if (onKey) {
        activePath_ = p;
        activeKey_ = k;
        if (modifiers & kStrokeCtrl) {
            deleteKey(host, settings);
            return true;
        }
        if (modifiers & kStrokeShift) {
            // Shift: the key's reach, out to the cursor as it drags.
            const MeshPtr mesh = keyMesh(host);
            if (!view.project(lengthKeyPosition(paths_.paths[p].keys[k], mesh.get()), keyX_, keyY_)) return true;
            drag_ = Drag::Reach;
            return true;
        }
        drag_ = Drag::Move;
        return true;
    }
    LengthKey key;
    if (!scalpPoint(host, view, x, y, key)) return false;
    if (!(modifiers & kStrokeShift) && segmentAt(host, view, x, y, &p, &k)) {
        // On a path's line: a key there, its length the path's at that spot (nothing changes).
        LengthPath& path = paths_.paths[p];
        const LengthSample at = sampleLengthPath(path, key.point, keyMesh(host).get());
        key.length = at.length > 0.0f ? at.length : path.keys[k - 1].length;
        // Its reach: the two keys' blended by how far along it lies between them.
        const Vec3f a = lengthKeyPosition(path.keys[k - 1], keyMesh(host).get());
        const Vec3f b = lengthKeyPosition(path.keys[k], keyMesh(host).get());
        const float ab = (b - a).length();
        const float t = ab > 0.0f ? std::clamp((key.point - a).dot(b - a) / (ab * ab), 0.0f, 1.0f) : 0.0f;
        key.reach = path.keys[k - 1].reach + (path.keys[k].reach - path.keys[k - 1].reach) * t;
        path.keys.insert(path.keys.begin() + k, key);
        activePath_ = p;
        activeKey_ = k;
        drag_ = Drag::Move;
        preview(host, settings);
        return true;
    }
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const bool fresh = startNew_ || (modifiers & kStrokeShift) || activePath_ < 0 ||
                       activePath_ >= static_cast<int>(paths_.paths.size());
    if (fresh) {
        paths_.paths.push_back(LengthPath{});
        activePath_ = static_cast<int>(paths_.paths.size()) - 1;
        startNew_ = false;
    }
    LengthPath& path = paths_.paths[activePath_];
    if (path.keys.empty()) {
        key.reach = worldRadiusAt(view, key.point, x, y, settings.screenRadius);
        if (!(key.reach > 0.0f)) key.reach = 1.0f;
    } else {
        key.reach = path.keys.back().reach;
    }
    key.length = h ? averageGuideLength(*h, targets(host, settings, *h), key.point, key.reach) : 0.0f;
    if (!(key.length > 0.0f)) key.length = path.keys.empty() ? 1.0f : path.keys.back().length;
    path.keys.push_back(key);
    activeKey_ = static_cast<int>(path.keys.size()) - 1;
    drag_ = Drag::Move;
    preview(host, settings);
    return true;
}

void LengthPathTool::drag(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y) {
    if (drag_ == Drag::None || activePath_ < 0 || activeKey_ < 0) return;
    LengthKey& key = paths_.paths[activePath_].keys[activeKey_];
    if (drag_ == Drag::Length) {
        key.length = startLength_ * std::exp2((y - startY_) / kPixelsPerDoubling);
    } else if (drag_ == Drag::Reach) {
        const float pixels = std::hypot(x - keyX_, y - keyY_);
        const float reach = worldRadiusAt(view, lengthKeyPosition(key, keyMesh(host).get()), keyX_, keyY_, pixels);
        if (!(reach > 0.0f)) return;
        key.reach = reach;
    } else {
        LengthKey moved;
        if (!scalpPoint(host, view, x, y, moved)) return;
        key.point = moved.point;
        key.root = moved.root;
    }
    preview(host, settings);
}

void LengthPathTool::release(StrokeHost&, const BrushToolSettings&) { drag_ = Drag::None; }

void LengthPathTool::select(StrokeHost& host, int path, int key) {
    load(host);
    if (path < 0 || path >= static_cast<int>(paths_.paths.size())) {
        activePath_ = activeKey_ = -1;
        return;
    }
    activePath_ = path;
    const int keys = static_cast<int>(paths_.paths[path].keys.size());
    activeKey_ = keys == 0 ? -1 : std::clamp(key, 0, keys - 1);
}

void LengthPathTool::setKeyLength(StrokeHost& host, const BrushToolSettings& settings, float length) {
    load(host);
    if (activePath_ < 0 || activeKey_ < 0 || !(length > 0.0f)) return;
    paths_.paths[activePath_].keys[activeKey_].length = length;
    preview(host, settings);
}

void LengthPathTool::setReach(StrokeHost& host, const BrushToolSettings& settings, float reach) {
    load(host);
    if (activePath_ < 0 || activeKey_ < 0 || !(reach > 0.0f)) return;
    paths_.paths[activePath_].keys[activeKey_].reach = reach;
    preview(host, settings);
}

void LengthPathTool::setSmooth(StrokeHost& host, const BrushToolSettings& settings, bool smooth) {
    load(host);
    if (activePath_ < 0) return;
    paths_.paths[activePath_].smooth = smooth;
    preview(host, settings);
}

void LengthPathTool::setChange(StrokeHost& host, const BrushToolSettings& settings, LengthChange to) {
    load(host);
    change = to;
    if (edited_) preview(host, settings);
}

void LengthPathTool::newPath() { startNew_ = true; }

void LengthPathTool::deleteKey(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (activePath_ < 0 || activeKey_ < 0) return;
    auto& keys = paths_.paths[activePath_].keys;
    keys.erase(keys.begin() + activeKey_);
    if (keys.empty()) {
        deletePath(host, settings);
        return;
    }
    activeKey_ = std::min(activeKey_, static_cast<int>(keys.size()) - 1);
    preview(host, settings);
}

void LengthPathTool::deletePath(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (activePath_ < 0) return;
    paths_.paths.erase(paths_.paths.begin() + activePath_);
    activePath_ = std::min(activePath_, static_cast<int>(paths_.paths.size()) - 1);
    activeKey_ = activePath_ >= 0 ? static_cast<int>(paths_.paths[activePath_].keys.size()) - 1 : -1;
    preview(host, settings);
}

bool LengthPathTool::apply(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (!edited_) return false;
    if (!host.prepareStroke()) return false;
    const std::shared_ptr<GuideHierarchy> h = result(host, settings);
    StrokeEdit edit;
    edit.mode = "lengths";
    edit.set(StrokeField::Guides, h->toJson());
    host.previewHierarchy(nullptr, false);
    host.commit(std::move(edit));
    edited_ = false;
    return true;
}

void LengthPathTool::cancel(StrokeHost& host) {
    if (edited_) host.previewHierarchy(nullptr, false);
    const int path = activePath_, key = activeKey_;
    loaded_ = false;
    load(host);
    select(host, path, key);
}

LengthPathTool::Overlay LengthPathTool::overlay(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    Overlay out;
    const MeshPtr mesh = keyMesh(host);
    const MeshPtr shown = host.scalp();
    // A key's point on the scalp as shown (refined: the nearest point of it to the key's cage point).
    auto at = [&](const LengthKey& key) {
        const Vec3f p = lengthKeyPosition(key, mesh.get());
        if (!shown || shown == mesh || shown->triangleCount() == 0) return p;
        const SurfacePoint hit = shown->closestPoint(p);
        return hit.root.face >= 0 ? hit.position : p;
    };
    out.activePath = activePath_;
    auto add = [&](const LengthPathSet& set, bool mirror) {
        for (std::size_t p = 0; p < set.paths.size(); ++p) {
            const LengthPath& path = set.paths[p];
            std::vector<Vec3f> line;
            for (std::size_t k = 0; k < path.keys.size(); ++k) {
                Overlay::Key key;
                key.at = at(path.keys[k]);
                key.length = path.keys[k].length;
                key.reach = path.keys[k].reach;
                key.path = static_cast<int>(p);
                key.mirror = mirror;
                key.active = !mirror && static_cast<int>(p) == activePath_ && static_cast<int>(k) == activeKey_;
                key.hovered = !mirror && static_cast<int>(p) == hoverPath_ && static_cast<int>(k) == hoverKey_;
                out.keys.push_back(key);
                line.push_back(key.at);
            }
            if (line.size() > 1) {
                out.lines.push_back(std::move(line));
                out.mirrorLines.push_back(mirror);
            }
        }
    };
    add(paths_, false);
    add(mirroredPaths(paths_, settings, mesh), true);
    return out;
}

std::vector<LengthPathTool::ColoredLine> LengthPathTool::overlayLines(StrokeHost& host, const ToolView& view,
                                                                     const BrushToolSettings& settings, float x, float y) {
    hover(host, view, x, y);
    const Overlay o = overlay(host, settings);
    std::vector<ColoredLine> out;
    const std::array<float, 3> blue{0.35f, 0.75f, 1.0f}, dim{0.18f, 0.38f, 0.5f}, active{1.0f, 0.85f, 0.2f}, white{1.0f, 1.0f, 1.0f};
    for (std::size_t i = 0; i < o.lines.size(); ++i) {
        const bool mirror = i < o.mirrorLines.size() && o.mirrorLines[i];
        out.push_back({o.lines[i], mirror ? dim : blue});
    }
    Vec3f eye, along;
    bool ortho = false;
    if (!view.camera(eye, along, ortho)) return out;
    auto ring = [&](const Vec3f& center, float radius, const std::array<float, 3>& color) {
        const Vec3f n = ortho ? along : (center - eye).normalized();
        const Vec3f u = (std::abs(n.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f}).cross(n).normalized();
        const Vec3f v = n.cross(u);
        ColoredLine line;
        line.color = color;
        constexpr int kSegments = 32;
        for (int k = 0; k <= kSegments; ++k) {
            const float a = 6.28318530718f * static_cast<float>(k) / kSegments;
            line.points.push_back(center + u * (std::cos(a) * radius) + v * (std::sin(a) * radius));
        }
        out.push_back(std::move(line));
    };
    for (const Overlay::Key& k : o.keys) {
        float kx = 0.0f, ky = 0.0f;
        if (!view.project(k.at, kx, ky)) continue;
        const float pixels = k.active || k.hovered ? 7.0f : k.mirror ? 4.0f : 5.0f;
        const float dot = worldRadiusAt(view, k.at, kx, ky, pixels);
        const std::array<float, 3> color = k.mirror ? dim : k.active ? active : k.hovered ? white : blue;
        if (dot > 0.0f) {
            ring(k.at, dot, color);
            ring(k.at, dot * 0.5f, color);
        }
        if (k.path == o.activePath && k.reach > 0.0f) ring(k.at, k.reach, k.mirror ? dim : blue);
    }
    return out;
}

}  // namespace groomlab
