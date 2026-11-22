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
    // A shaping brush's Ctrl+Shift drag: the selection shown as it is painted; at its end every
    // other guide frozen (none: all unfrozen), one undo step, as the Select tool's solo.
    bool paintSelection(const std::vector<GuideId>& selection, bool final) override {
        document_.setSelection(selection);
        if (final) {
            const std::shared_ptr<const GuideHierarchy> authored = document_.session().authoredHierarchy();
            GuideHierarchy probe = authored ? *authored : GuideHierarchy{};
            if (freezeToSelection(probe, selection)) {
                document_.editGuides("solo selection", [&](GuideHierarchy& g) { freezeToSelection(g, selection); });
            }
        }
        return true;
    }
    std::set<std::string> hiddenGroups() override { return document_.hiddenGroups(); }
    std::string mixedView() override { return document_.mixedView(); }
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


BrushSession::BrushSession() = default;  // (the settings start at the core's defaults, as every host's)

BrushSession::~BrushSession() = default;

namespace {
constexpr const char* kFreezeTool = "freeze";
}  // namespace

void BrushSession::setTool(const std::string& id) {
    // Every host's pick of a tool (the editor's pickTool, tool_settings.h): Freeze sets Paint's
    // settings aside and gives them back.
    if (tool_.settings.tool == BrushToolSettings::Tool::Roots && id != "roots" && id != kFreezeTool) {
        BrushToolSettings().setMode(id);  // throws for an unknown id, before the root mode is put away
        leaveRoots();
    }
    pickTool(tool_.settings, id, beforeFreeze_);
}

std::string BrushSession::toolId() const {
    if (tool_.settings.tool == BrushToolSettings::Tool::Roots) return "shape";  // the Tube tool's root mode
    return pickedTool(tool_.settings);  // (every host's: tool_settings.h)
}

bool BrushSession::startRoots(RootTubeTool::Mode mode) {
    if (!host_) {
        error_ = "no groom open";
        return false;
    }
    if (tool_.stroking()) tool_.cancel(host_.get());
    guard("roots", [&] {
        tool_.settings.setMode("roots");
        tool_.roots().start(*host_, mode);
    });
    refreshRoots();
    return error_.empty();
}

void BrushSession::leaveRoots() {
    if (host_ && tool_.roots().active()) guard("roots", [&] { tool_.roots().leave(*host_); });
    if (tool_.settings.tool == BrushToolSettings::Tool::Roots) tool_.settings.setMode("shape");
    refreshRoots();
}

bool BrushSession::editRoots(const char* what, const std::function<void(RootTubeTool&, StrokeHost&)>& fn) {
    if (!host_) {
        error_ = "no groom open";
        return false;
    }
    guard(what, [&] { fn(tool_.roots(), *host_); });
    refreshRoots();
    return error_.empty();
}

void BrushSession::refreshRoots() {
    RootOverlayGeometry g;
    if (host_ && tool_.settings.tool == BrushToolSettings::Tool::Roots && tool_.roots().active()) {
        try {
            const MeshPtr scalp = host_->scalp();
            g = rootOverlayGeometry(tool_.roots().overlay(*host_), scalp.get());
        } catch (const std::exception& e) {
            error_ = std::string("roots: ") + e.what();
        }
    }
    if (g.empty() && rootOverlay_.empty()) return;
    rootOverlay_ = std::move(g);
    ++overlayVersion_;
}

void BrushSession::setDocument(GroomDocument* document) {
    if (tool_.stroking()) tool_.cancel(host_.get());
    if (host_ && tool_.roots().active()) {
        try {
            tool_.roots().leave(*host_);  // (an edit not finished given to its roots)
        } catch (const std::exception&) {
        }
    }
    if (tool_.settings.tool == BrushToolSettings::Tool::Roots) tool_.settings.setMode("shape");
    rootOverlay_ = RootOverlayGeometry();
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
    pressIndex_ = document_ ? document_->undoIndex() : 0;
    pressText_ = document_ ? document_->undoText() : std::string();
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

bool BrushSession::editLengths(const char* what,
                               const std::function<void(LengthKeyTool&, StrokeHost&, const BrushToolSettings&)>& fn) {
    if (!host_) {
        error_ = "no groom open";
        return false;
    }
    guard(what, [&] { fn(tool_.lengths(), *host_, tool_.settings); });
    return error_.empty();
}

bool BrushSession::committedSincePress() const {
    return document_ && (document_->undoIndex() != pressIndex_ || document_->undoText() != pressText_);
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
    if (s.tool == Tool::Roots) {
        // The root mode: no ring, its outlines (Duplicate: the copies follow the cursor).
        if (withLines && host_ && tool_.roots().active() && !tool_.rootsPressed()) {
            guard("roots", [&] { tool_.roots().hover(*host_, view_, p[0], p[1]); });
        }
        const std::uint64_t before = overlayVersion_;
        refreshRoots();
        if (overlayVersion_ == before) ++overlayVersion_;
        return;
    }
    // Every host's overlay (the editor's brushOverlay, tool_overlay.h): the guides the brush would
    // touch, the line of symmetry, the ring (on the scalp, else at the camera target's depth; the
    // shaping brushes' in the theme's tone, a held key's state on it) and a Part or Flow path.
    OverlayRequest request;
    request.x = p[0];
    request.y = p[1];
    request.withLines = withLines;
    request.hint = hoverState_ == 1 ? RingHint::Reverse : hoverState_ == 2 ? RingHint::Smooth : RingHint::None;
    request.tone = ringToneSet_ ? &ringTone_ : nullptr;
    request.offScalp = [this](float x, float y, Vec3f& at) { return atTargetDepth({x, y}, at); };
    BrushOverlay drawn;
    guard("hover", [&] { drawn = brushOverlay(tool_, host_.get(), view_, request, seam_); });
    overlay_ = std::move(drawn.lines);
    ringValid_ = drawn.ring;
    ringCenter_ = drawn.ringCenter;
    ringRadius_ = drawn.ringRadius;
    if (s.tool == Tool::Lengths && host_) appendLengths(p);
    ++overlayVersion_;
}

void BrushSession::appendLengths(const std::array<float, 2>& p) {
    std::vector<LengthKeyTool::ColoredLine> lines;
    guard("lengths", [&] { lines = tool_.lengths().overlayLines(*host_, view_, tool_.settings, p[0], p[1]); });
    for (const LengthKeyTool::ColoredLine& line : lines) appendColoredLine(overlay_, line.points, line.color);
}

std::shared_ptr<const GuideHierarchy> BrushSession::shownHierarchy() const {
    return host_ ? host_->shownHierarchy() : nullptr;
}

std::size_t BrushSession::select(const std::vector<QPointF>& lasso, SelectMode mode, bool solo) {
    if (!document_ || !host_ || !view_.projection.valid() || lasso.empty()) return 0;
    const std::shared_ptr<const GuideHierarchy> h = host_->shownHierarchy();
    if (!h) return 0;
    // Every host's pick (the editor's pickGuidesInView, tool_select.h): a click, a box or a lasso,
    // only what the view shows, the mirror guides with symmetry.
    const BrushToolSettings& s = tool_.settings;
    GuidePick pick;
    pick.level = s.brush.level;
    pick.group = s.brush.group;
    pick.hiddenGroups = document_->hiddenGroups();
    const MeshPtr scalp = host_->scalp();
    pick.symmetry = StrokeSymmetry::of(s.brush, scalp.get());
    std::vector<std::array<float, 2>> pixels;
    for (const QPointF& p : lasso) pixels.push_back(port(p));
    const std::vector<GuideId> picked = pickGuidesInView(*h, view_, scalp, pixels, pick);
    const std::vector<GuideId> selection = combineSelection(document_->selection(), picked, mode);
    document_->setSelection(selection);
    // Solo: every guide not selected frozen, an undo step when that changed anything (a pick of
    // nothing with a modifier changes nothing; a plain one unfreezes all).
    if (solo && pickSoloes(picked, mode)) {
        const std::shared_ptr<const GuideHierarchy> authored = document_->session().authoredHierarchy();
        GuideHierarchy probe = authored ? *authored : GuideHierarchy{};
        const std::string layer = selectHides ? std::string(kHiddenLayer) : std::string("freeze");
        if (freezeToSelection(probe, selection, layer)) {
            document_->editGuides(selectHides ? "hide unselected guides" : "solo selection",
                                  [&](GuideHierarchy& g) { freezeToSelection(g, selection, layer); });
        }
    }
    return picked.size();
}

void BrushSession::warm(const ViewRequest& request) {
    if (!document_) return;
    ViewRequest r = request;
    r.sculptDensity = sculptDensity;
    document_->session().warmStroke(r);
}

}  // namespace groomlab::app
