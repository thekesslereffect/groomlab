// SPDX-License-Identifier: Apache-2.0
#include "brush_session.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

#include "groom_document.h"

namespace groomlab::app {

// The host over the document's session: the selection and hidden groups are the document's, the
// previews go to the session (SessionStrokeHost), and the stroke's edit goes to the document's
// history (one undo step beside the parameter edits), never to the session host's own list.
class DocumentStrokeHost : public SessionStrokeHost {
public:
    explicit DocumentStrokeHost(GroomDocument& document) : SessionStrokeHost(document.session()), document_(document) {}

    std::vector<GuideId> selectedGuides() override { return document_.selection(); }
    std::set<std::string> hiddenGroups() override { return document_.hiddenGroups(); }
    void commit(StrokeEdit edit) override { document_.commitStroke(std::move(edit)); }
    // Texture maps paint the document's paints (opened from the files beside the groom, written
    // back after each stroke): the session host's own list stays empty.
    std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override {
        return document_.texturePaint(name, channels, resolution);
    }
    void dropTexturePaint() override { document_.dropTexturePaint(); }

private:
    GroomDocument& document_;
};

namespace {

constexpr float kPi = 3.14159265358979f;

void appendColoredLine(DisplayGeometry& g, const std::vector<Vec3f>& points, const std::array<float, 3>& color) {
    if (points.size() < 2) return;
    const auto first = static_cast<std::uint32_t>(g.positions.size() / 3);
    for (const Vec3f& p : points) {
        g.positions.insert(g.positions.end(), {p.x, p.y, p.z});
        g.colors.insert(g.colors.end(), {color[0], color[1], color[2], 1.0f});
    }
    for (std::uint32_t k = 0; k + 1 < points.size(); ++k) g.indices.insert(g.indices.end(), {first + k, first + k + 1});
}

}  // namespace


BrushSession::BrushSession() {
    tool_.settings.brush.radius = 0.25f;
    tool_.settings.screenRadius = 60.0f;
    tool_.settings.place.soloNewGuides = false;  // off, as Maya's and Blender's Groom Tools start (Shift+click solos)
}

BrushSession::~BrushSession() = default;

namespace {
constexpr const char* kFreezeTool = "freeze";
}  // namespace

void BrushSession::setTool(const std::string& id) {
    BrushToolSettings& s = tool_.settings;
    const bool freezing = toolId() == kFreezeTool;
    if (id == kFreezeTool) {
        if (!freezing) beforeFreeze_ = {s.brush.attribute, s.brush.value, s.paintKind, s.paintTexture};
        s.setMode("paint");
        // The mask the brushes are held back by (BrushSettings::freeze), on the guides.
        s.brush.freeze = kFreezeTool;
        s.brush.attribute = kFreezeTool;
        s.brush.value = 1.0f;
        s.paintKind = "guides";
        s.paintTexture.clear();
        return;
    }
    BrushToolSettings().setMode(id);  // throws for an unknown id, before anything changes
    if (freezing) {
        s.brush.attribute = beforeFreeze_.attribute;
        s.brush.value = beforeFreeze_.value;
        s.paintKind = beforeFreeze_.kind;
        s.paintTexture = beforeFreeze_.texture;
    }
    s.setMode(id);
}

std::string BrushSession::toolId() const {
    const BrushToolSettings& s = tool_.settings;
    if (s.tool == BrushToolSettings::Tool::Brush && s.brush.mode == BrushMode::Attribute && s.brush.attribute == kFreezeTool &&
        s.paintKind == "guides") {
        return kFreezeTool;
    }
    return s.modeName();
}

void BrushSession::setDocument(GroomDocument* document) {
    if (tool_.stroking()) tool_.cancel(host_.get());
    document_ = document;
    host_ = document ? std::make_unique<DocumentStrokeHost>(*document) : nullptr;
    if (host_) host_->setShotSculpt(shot_, shotSeconds_, shotFrame_, shotLayer_);
    clearHover();
    error_.clear();
}

void BrushSession::setShotSculpt(bool on, double seconds, long frame, const std::string& layer) {
    shot_ = on && !layer.empty();
    shotSeconds_ = seconds;
    shotFrame_ = frame;
    shotLayer_ = shot_ ? layer : std::string();
    if (host_) host_->setShotSculpt(shot_, shotSeconds_, shotFrame_, shotLayer_);
}

void BrushSession::setView(const OrbitCamera& camera, int width, int height) {
    width_ = width;
    height_ = height;
    target_ = camera.target();
    view_ = toolViewFor(camera, width, height);
}

void BrushSession::guard(const char* what, const std::function<void()>& fn) {
    // The core's tools throw on bad data (a scalp without triangles, a texture map to paint
    // that is not there): the stroke is dropped, not the app.
    try {
        fn();
        error_.clear();
    } catch (const std::exception& e) {
        tool_.cancel(host_.get());
        error_ = std::string(what) + ": " + e.what();
    }
}

bool BrushSession::press(const QPointF& at, float pressure, unsigned modifiers) {
    if (!host_ || !view_.projection.valid()) {
        error_ = host_ ? "no view to brush in" : "no groom open to brush";
        return false;
    }
    const std::array<float, 2> p = port(at);
    guard("press", [&] { tool_.press(*host_, view_, p[0], p[1], pressure, modifiers); });
    rebuildOverlay(at, false);
    return error_.empty();
}

void BrushSession::move(const QPointF& at, float pressure) {
    if (!host_) return;
    const std::array<float, 2> p = port(at);
    guard("drag", [&] { tool_.drag(*host_, view_, p[0], p[1], pressure); });
    rebuildOverlay(at, false);
}

StrokeRelease BrushSession::release() {
    if (!host_) return StrokeRelease::None;
    StrokeRelease released = StrokeRelease::None;
    guard("release", [&] { released = tool_.release(*host_, view_); });
    return released;
}

void BrushSession::cancel() {
    if (tool_.stroking()) tool_.cancel(host_.get());
}

bool BrushSession::stroke(const std::vector<QPointF>& points, float pressure, unsigned modifiers) {
    if (points.empty() || !press(points.front(), pressure, modifiers)) return false;
    for (std::size_t i = 1; i < points.size(); ++i) move(points[i], pressure);
    const bool changed = tool_.changed();
    release();
    return changed && error_.empty();
}

int BrushSession::pickLevel(const QPointF& at) {
    if (!host_ || !view_.projection.valid()) return -2;
    const std::array<float, 2> p = port(at);
    int level = -2;
    guard("pick", [&] { level = tool_.pickLevel(*host_, view_, p[0], p[1]); });
    return level;
}

std::vector<std::string> BrushSession::takeWarnings() {
    if (!host_) return {};
    std::vector<std::string> out = host_->warnings();
    host_->clearWarnings();
    return out;
}

void BrushSession::hover(const QPointF& at) {
    if (!host_ || !view_.projection.valid()) {
        clearHover();
        return;
    }
    rebuildOverlay(at, !tool_.stroking());
}

void BrushSession::clearHover() {
    if (overlay_.empty() && !ringValid_) return;
    overlay_ = DisplayGeometry();
    overlay_.overlay = true;
    ringValid_ = false;
    ++overlayVersion_;
}

bool BrushSession::atTargetDepth(const std::array<float, 2>& pixel, Vec3f& out) const {
    Vec3f origin, direction, eye, along;
    bool ortho = false;
    if (!view_.ray(pixel[0], pixel[1], origin, direction) || !view_.camera(eye, along, ortho)) return false;
    const Vec3f target{target_.x(), target_.y(), target_.z()};
    const float t = (target - origin).dot(along) / std::max(direction.dot(along), 1e-6f);
    if (t <= 0.0f) return false;
    out = origin + direction * t;
    return true;
}

void BrushSession::showBox(const QPointF& a, const QPointF& b) {
    overlay_ = DisplayGeometry();
    overlay_.overlay = true;
    ringValid_ = false;
    const std::array<float, 2> pa = port(a), pb = port(b);
    std::vector<Vec3f> corners(5);
    const std::array<std::array<float, 2>, 5> pixels{{{pa[0], pa[1]}, {pb[0], pa[1]}, {pb[0], pb[1]}, {pa[0], pb[1]}, {pa[0], pa[1]}}};
    bool ok = true;
    for (std::size_t k = 0; k < 5 && ok; ++k) ok = atTargetDepth(pixels[k], corners[k]);
    if (ok) appendColoredLine(overlay_, corners, {0.9f, 0.9f, 0.9f});
    ++overlayVersion_;
}

void BrushSession::rebuildOverlay(const QPointF& at, bool withLines) {
    overlay_ = DisplayGeometry();
    overlay_.overlay = true;
    ringValid_ = false;
    const std::array<float, 2> p = port(at);
    const BrushToolSettings& s = tool_.settings;
    using Tool = BrushToolSettings::Tool;
    if (withLines && host_) {
        // The guides the brush would touch, as the core draws them for the hover (smoothed).
        std::vector<std::vector<Vec3f>> lines;
        guard("hover", [&] { lines = tool_.affectedLines(*host_, view_, p[0], p[1]); });
        for (const std::vector<Vec3f>& line : lines) appendColoredLine(overlay_, line, brushHoverColor(s));
    }
    // Symmetry on: the scalp's line of symmetry in red, as Substance Painter shows its center line.
    if (host_) {
        const BrushSettings& b = s.brush;
        const int axis = b.mirrorTopology ? -1 : b.mirrorAxis;
        const MeshPtr scalp = (b.mirrorTopology || (axis >= 0 && axis <= 2)) ? host_->scalp() : nullptr;
        if (scalp && scalp->triangleCount() > 0) {
            if (seam_.scalp != scalp || seam_.axis != axis || seam_.center != b.mirrorCenter || seam_.topology != b.mirrorTopology) {
                Vec3f lo = scalp->points().front(), hi = lo;
                for (const Vec3f& q : scalp->points()) {
                    lo = Vec3f(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
                    hi = Vec3f(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
                }
                seam_ = {scalp, axis, b.mirrorCenter, b.mirrorTopology,
                         symmetrySeam(*scalp, StrokeSymmetry::of(b, scalp.get()), 0.002f * (hi - lo).length())};
            }
            for (std::size_t i = 0; i + 1 < seam_.points.size(); i += 2) {
                appendColoredLine(overlay_, {seam_.points[i], seam_.points[i + 1]}, {1.0f, 0.15f, 0.15f});
            }
        }
    }
    // The ring: on the scalp under the cursor, else at the camera target's depth, as wide as the
    // screen radius there (a dot for Place and Draw: the click places one guide).
    Vec3f origin, direction;
    if (view_.ray(p[0], p[1], origin, direction)) {
        Vec3f center;
        bool have = false;
        const MeshPtr scalp = host_ ? host_->scalp() : nullptr;
        SurfacePoint hit;
        if (scalp && scalp->triangleCount() > 0 && scalp->intersectRay(origin, direction, &hit)) {
            center = hit.position;
            have = true;
        } else {
            have = atTargetDepth(p, center);
        }
        if (have) {
            const bool creating = s.tool == Tool::Place || s.tool == Tool::Draw;
            const float radius = worldRadiusAt(view_, center, p[0], p[1], creating ? 6.0f : s.screenRadius);
            if (radius > 0.0f) {
                Vec3f eye, along;
                bool ortho = false;
                view_.camera(eye, along, ortho);
                // In the plane facing the camera: the circle reads as the screen circle it is.
                const Vec3f n = ortho ? along : (center - eye).normalized();
                const Vec3f u = (std::abs(n.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f}).cross(n).normalized();
                const Vec3f v = n.cross(u);
                std::vector<Vec3f> ring;
                constexpr int kSegments = 48;
                for (int k = 0; k <= kSegments; ++k) {
                    const float a = 2.0f * kPi * static_cast<float>(k) / kSegments;
                    ring.push_back(center + u * (std::cos(a) * radius) + v * (std::sin(a) * radius));
                }
                appendColoredLine(overlay_, ring, brushRingColor(s));
                ringValid_ = true;
                ringCenter_ = center;
                ringRadius_ = radius;
            }
        }
    }
    // A Part or Flow stroke's path so far, until it is committed.
    if (tool_.stroking() && (s.tool == Tool::Part || s.tool == Tool::Flow)) {
        appendColoredLine(overlay_, tool_.drawnPath(), brushPathColor(s));
    }
    ++overlayVersion_;
}

std::shared_ptr<const GuideHierarchy> BrushSession::shownHierarchy() const {
    return host_ ? host_->shownHierarchy() : nullptr;
}

std::size_t BrushSession::select(const std::vector<QPointF>& lasso, SelectMode mode, bool solo) {
    if (!document_ || !host_ || !view_.projection.valid() || lasso.empty()) return 0;
    const std::shared_ptr<const GuideHierarchy> h = host_->shownHierarchy();
    if (!h) return 0;
    const BrushToolSettings& s = tool_.settings;
    const std::vector<GuideId> candidates = guidesAt(*h, s.brush.level, s.brush.group, document_->hiddenGroups());
    std::vector<std::array<float, 2>> polygon;
    if (lasso.size() == 2) {
        // A box from two corners.
        const std::array<float, 2> a = port(lasso[0]), b = port(lasso[1]);
        polygon = {{a[0], a[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}};
    } else {
        for (const QPointF& p : lasso) polygon.push_back(port(p));
    }
    std::vector<GuideId> picked =
        pickGuides(*h, candidates, pixelProjector(view_.projection), polygon, false, 12.0f, &view_.projection);
    // With symmetry, each picked guide's mirror guide too.
    const MeshPtr scalp = host_->scalp();
    picked = withMirrorGuides(*h, picked, StrokeSymmetry::of(s.brush, scalp.get()));
    const std::vector<GuideId> selection = combineSelection(document_->selection(), picked, mode);
    document_->setSelection(selection);
    // Solo: every guide not selected frozen, an undo step when that changed anything (a pick of
    // nothing with a modifier changes nothing; a plain one unfreezes all).
    if (solo && (!picked.empty() || mode == SelectMode::Replace)) {
        const std::shared_ptr<const GuideHierarchy> authored = document_->session().authoredHierarchy();
        GuideHierarchy probe = authored ? *authored : GuideHierarchy{};
        if (freezeToSelection(probe, selection)) {
            document_->editGuides("solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
        }
    }
    return picked.size();
}

std::size_t BrushSession::paintSelection(const QPointF& at) {
    std::vector<QPointF> ring;
    const double r = tool_.settings.screenRadius;
    for (int k = 0; k < 24; ++k) {
        const double a = 6.283185307 * k / 24.0;
        ring.push_back(at + QPointF(r * std::cos(a), r * std::sin(a)));
    }
    return select(ring, SelectMode::Add, false);
}

void BrushSession::soloSelection() {
    if (!document_) return;
    const std::vector<GuideId> selection = document_->selection();
    const std::shared_ptr<const GuideHierarchy> authored = document_->session().authoredHierarchy();
    GuideHierarchy probe = authored ? *authored : GuideHierarchy{};
    if (freezeToSelection(probe, selection)) {
        document_->editGuides("solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
    }
}

void BrushSession::warm(const ViewRequest& request) {
    if (!document_) return;
    ViewRequest r = request;
    r.sculptDensity = sculptDensity;
    document_->session().warmStroke(r);
}

}  // namespace groomlab::app
