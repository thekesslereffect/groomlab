// SPDX-License-Identifier: Apache-2.0
#include "groomlab/length_key_tool.h"

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

// The keys mirrored as the brush's symmetry mirrors (a world plane, or the scalp's topology: each
// key at its partner spot on the scalp); none when symmetry is off.
LengthKeySet mirroredKeys(const LengthKeySet& keys, const BrushToolSettings& settings, const MeshPtr& mesh) {
    const StrokeSymmetry sym = StrokeSymmetry::of(settings.brush, mesh.get());
    if (!sym.on()) return {};
    return mirroredLengthKeys(keys, mesh.get(), [&sym](const Vec3f& p) { return sym.point(p); });
}

}  // namespace

void LengthKeyTool::load(StrokeHost& host) {
    if (loaded_ && edited_) return;  // the edits stand until Apply or Cancel
    // Not edited: the stored keys, read again each time (an undo, a script, another groom's).
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    LengthKeySet stored = h ? h->lengthKeys() : LengthKeySet{};
    if (loaded_ && stored.toJson() == keys_.toJson()) return;
    const bool keep = loaded_ && stored.keys.size() == keys_.keys.size();
    keys_ = std::move(stored);
    loaded_ = true;
    edited_ = false;
    if (keep) {  // the same keys (applied: their scales back at x1): the same selection
        select(host, activeKey_);
        return;
    }
    activeKey_ = static_cast<int>(keys_.keys.size()) - 1;
}

const LengthKeySet& LengthKeyTool::keys(StrokeHost& host) {
    load(host);
    return keys_;
}

void LengthKeyTool::reset(StrokeHost* host) {
    if (host && edited_) host->previewHierarchy(nullptr, false);
    loaded_ = edited_ = false;
    keys_ = {};
    activeKey_ = hoverKey_ = -1;
    drag_ = Drag::None;
}

std::vector<GuideId> LengthKeyTool::targets(StrokeHost& host, const BrushToolSettings& settings, const GuideHierarchy& h) const {
    BrushSettings b = settings.brush;
    b.hiddenGroups = host.hiddenGroups();
    std::map<GuideId, float> selected;
    for (const GuideId id : host.selectedGuides()) selected[id] = 1.0f;
    b.mask = selected.empty() ? nullptr : &selected;
    return brushTargets(h, b);
}

LengthKeySet LengthKeyTool::acting(StrokeHost& host, const BrushToolSettings& settings) const {
    LengthKeySet both = keys_;
    const LengthKeySet mirrored = mirroredKeys(keys_, settings, keyMesh(host));
    both.keys.insert(both.keys.end(), mirrored.keys.begin(), mirrored.keys.end());
    return both;
}

std::shared_ptr<GuideHierarchy> LengthKeyTool::result(StrokeHost& host, const BrushToolSettings& settings) {
    const std::shared_ptr<const GuideHierarchy> stored = host.authoredHierarchy();
    auto h = std::make_shared<GuideHierarchy>(stored ? *stored : GuideHierarchy{});
    const MeshPtr mesh = keyMesh(host);
    const LengthField field(acting(host, settings), mesh.get());
    for (const LengthResult& r : applyLengthField(*h, targets(host, settings, *h), field, change)) h->setPoints(r.id, r.points);
    h->setLengthKeys(keys_);
    return h;
}

void LengthKeyTool::preview(StrokeHost& host, const BrushToolSettings& settings) {
    edited_ = true;
    host.previewHierarchy(result(host, settings), false);
}

bool LengthKeyTool::scalpPoint(StrokeHost& host, const ToolView& view, float x, float y, LengthKey& key) const {
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

int LengthKeyTool::keyAt(StrokeHost& host, const ToolView& view, float x, float y) {
    load(host);
    const MeshPtr mesh = keyMesh(host);
    float best = kPickPixels * kPickPixels;
    int found = -1;
    for (std::size_t k = 0; k < keys_.keys.size(); ++k) {
        float kx = 0.0f, ky = 0.0f;
        if (!view.project(lengthKeyPosition(keys_.keys[k], mesh.get()), kx, ky)) continue;
        const float d2 = (kx - x) * (kx - x) + (ky - y) * (ky - y);
        if (d2 <= best) {
            best = d2;
            found = static_cast<int>(k);
        }
    }
    return found;
}

void LengthKeyTool::hover(StrokeHost& host, const ToolView& view, float x, float y) { hoverKey_ = keyAt(host, view, x, y); }

bool LengthKeyTool::press(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y,
                          unsigned modifiers) {
    load(host);
    drag_ = Drag::None;
    const int onKey = keyAt(host, view, x, y);
    if (modifiers & kStrokeAdjust) {
        // The adjusting button: the key under the cursor's scale, else the selected key's.
        if (onKey >= 0) activeKey_ = onKey;
        if (activeKey_ < 0) return false;
        drag_ = Drag::Scale;
        startY_ = y;
        startScale_ = keys_.keys[activeKey_].scale;
        return true;
    }
    if (onKey >= 0) {
        activeKey_ = onKey;
        if (modifiers & kStrokeCtrl) {
            deleteKey(host, settings);
            return true;
        }
        drag_ = Drag::Move;
        return true;
    }
    LengthKey key;
    if (!scalpPoint(host, view, x, y, key)) return false;
    // The scale the field has there already (with symmetry's mirror), so the new key changes nothing.
    key.scale = LengthField(acting(host, settings), keyMesh(host).get()).scaleAt(key.point);
    keys_.keys.push_back(key);
    activeKey_ = static_cast<int>(keys_.keys.size()) - 1;
    drag_ = Drag::Move;
    preview(host, settings);
    return true;
}

void LengthKeyTool::drag(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y) {
    if (drag_ == Drag::None || activeKey_ < 0) return;
    LengthKey& key = keys_.keys[activeKey_];
    if (drag_ == Drag::Scale) {
        key.scale = startScale_ * std::exp2((y - startY_) / kPixelsPerDoubling);
    } else {
        LengthKey moved;
        if (!scalpPoint(host, view, x, y, moved)) return;
        key.point = moved.point;
        key.root = moved.root;
    }
    preview(host, settings);
}

void LengthKeyTool::release(StrokeHost&, const BrushToolSettings&) { drag_ = Drag::None; }

void LengthKeyTool::select(StrokeHost& host, int key) {
    load(host);
    const int n = static_cast<int>(keys_.keys.size());
    activeKey_ = n == 0 || key < 0 ? -1 : std::min(key, n - 1);
}

float LengthKeyTool::keyLength(StrokeHost& host, const BrushToolSettings& settings, int key) {
    load(host);
    if (key < 0 || key >= static_cast<int>(keys_.keys.size())) return 0.0f;
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    if (!h) return 0.0f;
    const Vec3f at = lengthKeyPosition(keys_.keys[key], keyMesh(host).get());
    return keys_.keys[key].scale * nearestGuideLength(*h, targets(host, settings, *h), at);
}

void LengthKeyTool::setKeyScale(StrokeHost& host, const BrushToolSettings& settings, float scale) {
    load(host);
    if (activeKey_ < 0 || !(scale > 0.0f)) return;
    keys_.keys[activeKey_].scale = scale;
    preview(host, settings);
}

void LengthKeyTool::setKeyLength(StrokeHost& host, const BrushToolSettings& settings, float length) {
    load(host);
    if (activeKey_ < 0 || !(length > 0.0f)) return;
    const float now = keyLength(host, settings, activeKey_) / keys_.keys[activeKey_].scale;
    if (now > 0.0f) setKeyScale(host, settings, length / now);
}

void LengthKeyTool::setChange(StrokeHost& host, const BrushToolSettings& settings, LengthChange to) {
    load(host);
    change = to;
    if (edited_) preview(host, settings);
}

void LengthKeyTool::deleteKey(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (activeKey_ < 0) return;
    keys_.keys.erase(keys_.keys.begin() + activeKey_);
    activeKey_ = std::min(activeKey_, static_cast<int>(keys_.keys.size()) - 1);
    preview(host, settings);
}

void LengthKeyTool::clear(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (keys_.empty()) return;
    keys_.keys.clear();
    activeKey_ = -1;
    preview(host, settings);
}

bool LengthKeyTool::apply(StrokeHost& host, const BrushToolSettings& settings) {
    load(host);
    if (!edited_) return false;
    if (!host.prepareStroke()) return false;
    const std::shared_ptr<GuideHierarchy> h = result(host, settings);
    // The scales are in the guides now: the keys stay where they are, at x1.
    LengthKeySet held = keys_;
    for (LengthKey& k : held.keys) k.scale = 1.0f;
    h->setLengthKeys(held);
    StrokeEdit edit;
    edit.mode = "lengths";
    edit.set(StrokeField::Guides, h->toJson());
    host.previewHierarchy(nullptr, false);
    host.commit(std::move(edit));
    edited_ = false;
    return true;
}

void LengthKeyTool::cancel(StrokeHost& host) {
    if (edited_) host.previewHierarchy(nullptr, false);
    const int key = activeKey_;
    loaded_ = false;
    load(host);
    select(host, key);
}

LengthKeyTool::Overlay LengthKeyTool::overlay(StrokeHost& host, const BrushToolSettings& settings) {
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
    for (std::size_t k = 0; k < keys_.keys.size(); ++k) {
        Overlay::Key key;
        key.at = at(keys_.keys[k]);
        key.scale = keys_.keys[k].scale;
        key.length = keyLength(host, settings, static_cast<int>(k));
        key.active = static_cast<int>(k) == activeKey_;
        key.hovered = static_cast<int>(k) == hoverKey_;
        out.keys.push_back(key);
    }
    const LengthKeySet mirrored = mirroredKeys(keys_, settings, mesh);
    for (std::size_t k = 0; k < mirrored.keys.size(); ++k) {
        Overlay::Key key;
        key.at = at(mirrored.keys[k]);
        key.scale = mirrored.keys[k].scale;
        key.length = out.keys[k].length;
        key.mirror = true;
        out.keys.push_back(key);
    }
    return out;
}

std::array<float, 3> LengthKeyTool::keyColor(const Overlay::Key& k) {
    if (k.active) return {1.0f, 0.85f, 0.2f};
    if (k.hovered) return {1.0f, 1.0f, 1.0f};
    std::array<float, 3> c = std::abs(k.scale - 1.0f) < 1e-3f ? std::array<float, 3>{0.7f, 0.7f, 0.7f}
                             : k.scale > 1.0f                  ? std::array<float, 3>{0.35f, 0.75f, 1.0f}
                                                               : std::array<float, 3>{1.0f, 0.55f, 0.25f};
    if (k.mirror) {
        for (float& v : c) v *= 0.5f;
    }
    return c;
}

std::vector<LengthKeyTool::ColoredLine> LengthKeyTool::overlayLines(StrokeHost& host, const ToolView& view,
                                                                   const BrushToolSettings& settings, float x, float y) {
    hover(host, view, x, y);
    const Overlay o = overlay(host, settings);
    std::vector<ColoredLine> out;
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
        const float dot = worldRadiusAt(view, k.at, kx, ky, k.mirror ? 3.0f : 4.0f);
        if (dot > 0.0f) {
            ring(k.at, dot, keyColor(k));
            ring(k.at, dot * 0.5f, keyColor(k));
        }
        if (k.mirror) continue;
        const float edge = worldRadiusAt(view, k.at, kx, ky, kPickPixels);
        if (edge > 0.0f) {
            ring(k.at, edge, keyColor(k));
            if (k.active || k.hovered) ring(k.at, edge * 1.06f, keyColor(k));  // (thicker)
        }
    }
    return out;
}

}  // namespace groomlab
