// SPDX-License-Identifier: Apache-2.0
// The interactive tool, scripted strokes, edits and the session's host (stroke.h).

#include "groomlab/portable_math.h"
#include "groomlab/stroke.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "groomlab/curve.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_selection.h"
#include "groomlab/random.h"
#include "groomlab/subdivision.h"

namespace groomlab {

// ---- Edits ------------------------------------------------------------------------------------

const char* strokeFieldName(StrokeField field) {
    switch (field) {
        case StrokeField::Guides: return "guideData";
        case StrokeField::Parts: return "partData";
        case StrokeField::Flow: return "flowData";
        case StrokeField::Correctives: return "correctiveData";
        case StrokeField::ScalpMaps: return "scalpMaps";
        case StrokeField::ShotLayer: return "shotLayer";
        case StrokeField::Graph: return "graph";
        case StrokeField::Sculpts: return "sculptData";
    }
    return "";
}

std::string sculptRefusal(const BrushToolSettings& s) {
    using Tool = BrushToolSettings::Tool;
    const char* why = "GroomLab sculpt: only shaping brushes (comb, smooth, lift, length, twist, curl, straighten, attract, "
                      "cut, direction, noise) shape a Sculpt node's guides (pick no Sculpt node to place, draw, erase, paint "
                      "or part)";
    if (s.tool != Tool::Brush || s.paintsScalp()) return why;
    switch (s.brush.mode) {
        case BrushMode::Comb:
        case BrushMode::Smooth:
        case BrushMode::Lift:
        case BrushMode::Length:
        case BrushMode::Twist:
        case BrushMode::Curl:
        case BrushMode::Straighten:
        case BrushMode::Attract:
        case BrushMode::Cut:
        case BrushMode::Direction:
        case BrushMode::Noise: return {};
        case BrushMode::ShapeTube:
        case BrushMode::Attribute:
        case BrushMode::Erase:
        case BrushMode::Slide: break;
    }
    return why;
}

bool StrokeEdit::empty() const {
    for (const Text& t : texts) {
        if (t.set) return false;
    }
    return textureMap.empty();
}

std::size_t StrokeEdit::bytes() const {
    std::size_t total = texture.bytes();
    for (const Text& t : texts) total += t.after.size() + t.before.size();
    return total;
}

void StrokeEdit::apply(GroomSession& session, bool after) const {
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        const Text& t = texts[i];
        if (!t.set || (!after && !t.hasBefore)) continue;
        const std::string& text = after ? t.after : t.before;
        switch (static_cast<StrokeField>(i)) {
            case StrokeField::Guides: session.setHierarchyJson(text); break;
            case StrokeField::Parts: session.setPartsJson(text); break;
            case StrokeField::Flow: session.setFlowJson(text); break;
            case StrokeField::Correctives: session.setCorrectivesJson(text); break;
            case StrokeField::ScalpMaps: session.setScalpMapsJson(text); break;
            case StrokeField::ShotLayer: break;  // the host's
            case StrokeField::Graph: session.setGraphJson(text); break;
            case StrokeField::Sculpts: session.setSculptsJson(text); break;
        }
    }
}

// ---- Parts and flow on the scalp shown -----------------------------------------------------------

OverlayBinding OverlayBinding::of(StrokeHost& host, const MeshPtr& scalp) {
    OverlayBinding b;
    b.scalp = scalp;
    if (scalp && scalp->subdivisionLevel() > 0) {
        b.cage = host.paintScalp();
        if (b.cage && b.cage != scalp) b.plan = refinementPlan(*b.cage, *scalp);
    }
    if (!b.plan) b.cage = scalp;
    return b;
}

PartSet OverlayBinding::shown(const PartSet& stored) const { return plan ? stored.onRefined(*plan) : stored; }
PartSet OverlayBinding::stored(const PartSet& shown) const { return plan ? shown.onCage(*plan) : shown; }
FlowSet OverlayBinding::shown(const FlowSet& stored) const { return plan ? stored.onRefined(*plan, *cage, *scalp) : stored; }
FlowSet OverlayBinding::stored(const FlowSet& shown) const { return plan ? shown.onCage(*plan, *scalp, *cage) : shown; }

// ---- Hosts ------------------------------------------------------------------------------------

std::shared_ptr<GuideHierarchy> StrokeHost::editableHierarchy() {
    const std::shared_ptr<const GuideHierarchy> h = authoredHierarchy();
    return std::make_shared<GuideHierarchy>(h ? *h : GuideHierarchy{});
}

SculptInput StrokeHost::sculptInput(const std::string& node) {
    throw std::runtime_error("this host cannot sculpt node " + node);
}

std::shared_ptr<TexturePaint> StrokeHost::texturePaint(const std::string& name, int, int) {
    throw std::runtime_error("this groom has no texture map named " + name);
}

SessionStrokeHost::SessionStrokeHost(GroomSession& session) : session_(session) {}

MeshPtr SessionStrokeHost::scalp() { return session_.scalp(); }
MeshPtr SessionStrokeHost::paintScalp() { return session_.cageScalp(); }

std::shared_ptr<const GuideHierarchy> SessionStrokeHost::authoredHierarchy() { return session_.authoredHierarchy(); }

std::shared_ptr<const GuideHierarchy> SessionStrokeHost::shownHierarchy() {
    if (!session_.sculptTarget().empty()) {
        if (const HierarchyPtr sculpted = session_.sculptHierarchy()) return sculpted;
    }
    return session_.hierarchy();
}

std::string SessionStrokeHost::sculptNode() { return session_.sculptTarget(); }

SculptInput SessionStrokeHost::sculptInput(const std::string& node) { return session_.sculptInput(node); }

void SessionStrokeHost::previewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown) {
    session_.setPreviewSculpt(node, std::move(edits), std::move(shown));
}

std::uint64_t SessionStrokeHost::shownHierarchyVersion() { return 0; }  // hover compares the pointer

std::shared_ptr<const GuideHierarchy> SessionStrokeHost::posedHierarchy() {
    session_.hierarchy();  // the active hierarchy (and the posed one) brought up to date
    return session_.posedHierarchy();
}

std::string SessionStrokeHost::storedText(StrokeField field) {
    switch (field) {
        case StrokeField::Guides: {
            // Guides handed over as a hierarchy (setHierarchy) have no text: theirs.
            const std::shared_ptr<const GuideHierarchy> stored = session_.storedHierarchy();
            return session_.data().hierarchyJson.empty() && stored ? stored->toJson() : session_.data().hierarchyJson;
        }
        case StrokeField::Parts: return session_.data().partsJson;
        case StrokeField::Flow: return session_.data().flowJson;
        case StrokeField::Correctives: return session_.data().correctivesJson;
        case StrokeField::ScalpMaps: return session_.scalpMapsJson();
        case StrokeField::ShotLayer: return shotLayer_;
        case StrokeField::Graph: return session_.data().graphJson;
        case StrokeField::Sculpts: return session_.data().sculptsJson;
    }
    return {};
}

const ScalpMaps& SessionStrokeHost::scalpMaps() { return session_.scalpMaps(); }

std::shared_ptr<TexturePaint> SessionStrokeHost::texturePaint(const std::string& name, int, int) {
    const auto it = paints_.find(name);
    if (it == paints_.end() || !it->second) {
        throw std::runtime_error("no paint for texture map " + name + " (SessionStrokeHost::setTexturePaint)");
    }
    return it->second;
}

void SessionStrokeHost::previewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) {
    session_.setPreviewHierarchy(std::move(preview), final);
}

void SessionStrokeHost::previewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) {
    session_.setPreviewOverlays(std::move(parts), std::move(flow));
}

void SessionStrokeHost::previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) {
    session_.setPreviewVertexMap(name, std::move(values), channels);
}

void SessionStrokeHost::previewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) {
    session_.setPreviewTextureMap(name, std::move(image));
}

void SessionStrokeHost::liveStroke(bool on) { session_.setLiveStroke(on); }

void SessionStrokeHost::setShotSculpt(bool on, double seconds, long frame, std::string layer) {
    shot_ = on;
    seconds_ = seconds;
    frame_ = frame;
    shotLayer_ = std::move(layer);
}

void SessionStrokeHost::setTexturePaint(const std::string& name, std::shared_ptr<TexturePaint> paint) {
    if (paint) {
        paints_[name] = std::move(paint);
    } else {
        paints_.erase(name);
    }
}

std::shared_ptr<TexturePaint> SessionStrokeHost::paintOf(const std::string& name) const {
    const auto it = paints_.find(name);
    return it == paints_.end() ? nullptr : it->second;
}

void SessionStrokeHost::applyEdit(const StrokeEdit& edit, bool after) {
    edit.apply(session_, after);
    const StrokeEdit::Text& layer = edit.text(StrokeField::ShotLayer);
    if (layer.set && (after || layer.hasBefore)) shotLayer_ = after ? layer.after : layer.before;
}

void SessionStrokeHost::commit(StrokeEdit edit) {
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        StrokeEdit::Text& t = edit.texts[i];
        if (!t.set) continue;
        t.before = storedText(static_cast<StrokeField>(i));
        t.hasBefore = true;
    }
    applyEdit(edit, true);  // a texture map's paint holds the stroke already
    done_.push_back(std::move(edit));
    undone_.clear();
}

bool SessionStrokeHost::undo() {
    if (done_.empty()) return false;
    StrokeEdit edit = std::move(done_.back());
    done_.pop_back();
    applyEdit(edit, false);
    if (!edit.texture.empty()) {
        if (const std::shared_ptr<TexturePaint> paint = paintOf(edit.textureMap)) paint->applyEdit(edit.texture, false);
    }
    undone_.push_back(std::move(edit));
    return true;
}

bool SessionStrokeHost::redo() {
    if (undone_.empty()) return false;
    StrokeEdit edit = std::move(undone_.back());
    undone_.pop_back();
    applyEdit(edit, true);
    if (!edit.texture.empty()) {
        if (const std::shared_ptr<TexturePaint> paint = paintOf(edit.textureMap)) paint->applyEdit(edit.texture, true);
    }
    done_.push_back(std::move(edit));
    return true;
}

// ---- The interactive tool -------------------------------------------------------------------

struct BrushTool::HoverCache {
    std::shared_ptr<const GuideHierarchy> hierarchy;
    std::uint64_t hierarchyVersion = 0, filter = 0, view = 0;
    std::vector<const HierarchyGuide*> guides;  // the brushable ones, in the hierarchy's order
    std::unique_ptr<ScreenPointGrid> grid;      // their points on screen
    int smoothing = -1;
    std::vector<std::vector<Vec3f>> lines;  // per guide: as drawn (smoothed), once lit
    std::vector<char> made;
};

BrushTool::BrushTool() = default;
BrushTool::~BrushTool() = default;

std::string BrushTool::toolModeName() const {
    if (!shiftSmoothing_) return settings.modeName();
    BrushToolSettings own;
    own.tool = settings.tool;
    own.brush.mode = savedMode_;
    own.brush.attribute = settings.brush.attribute;
    return own.modeName();
}

std::shared_ptr<const ScalpVisibility> BrushTool::visibility(const MeshPtr& scalp, const ToolView& view) {
    Vec3f eye, along;
    bool ortho = false;
    if (!settings.maskByScalp || !scalp || !view.camera(eye, along, ortho)) {
        visibility_.reset();
        return nullptr;
    }
    if (!visibility_ || visibility_->key() != ScalpVisibility::keyOf(*scalp, eye, along, ortho)) {
        visibility_ = std::make_shared<const ScalpVisibility>(scalp, eye, along, ortho);
    }
    return visibility_;
}

BrushWeightFn BrushTool::screenWeight(const ToolView& view, float x, float y) const {
    // Screen-space falloff around the cursor; nothing the scalp hides (maskByScalp).
    return view.brushWeight(x, y, std::max(settings.screenRadius, 1.0f), falloffOf(settings.brush), visibility_);
}

BrushSettings BrushTool::pressed() const {
    BrushSettings b = settings.brush;
    if (sculpt_) {  // a Sculpt node's guides: one root group, all of it brushable
        b.level = 0;
        b.group.clear();
    }
    if (settings.pressure) b.strength *= pressure_;
    if (inverted_) {
        // Ctrl: the opposite action. The brushes that work both ways flip (Length shortens, Lift
        // lays down, Twist and Curl turn back, Attract pushes apart, Direction turns away, Noise
        // smooths, Shape shrinks, rounds or untwists); a gray map is painted toward 0 (unfreeze,
        // erase a layer, no hair on a density map, thinner on a width map), or toward 1 when the
        // value is 0 (grayCtrlTarget; a color map's inverse color is MapDab's); Cut grows instead.
        // Comb, Slide, Smooth, Straighten and Erase have no opposite: Ctrl leaves them as they are.
        switch (b.mode) {
            case BrushMode::Attribute: b.value = grayCtrlTarget(b.value); break;
            case BrushMode::Cut: b.mode = BrushMode::Length; break;
            case BrushMode::Lift:
            case BrushMode::Length:
            case BrushMode::ShapeTube:
            case BrushMode::Twist:
            case BrushMode::Curl:
            case BrushMode::Attract:
            case BrushMode::Direction:
            case BrushMode::Noise: b.strength = -b.strength; break;
            default: break;
        }
    }
    return b;
}

void BrushTool::rebuildDrawn() {
    *working_ = *base_;
    try {
        drawWithSymmetry(*working_, *scalp_, settings, drawPath_);
        changed_ = true;
    } catch (const std::invalid_argument&) {
        changed_ = false;  // too short so far
    }
}

bool BrushTool::anchorAt(const ToolView& view, float x, float y, const Vec3f& origin, const Vec3f& direction,
                         Vec3f& anchor) const {
    GuidePointHit hit;
    if (nearestGuidePoint(*working_, view, x, y, std::max(settings.screenRadius, 1.0f), origin, direction, visibility_.get(), &hit)) {
        anchor = hit.point;
        return true;
    }
    SurfacePoint onScalp;
    if (!scalp_->intersectRay(origin, direction, &onScalp)) return false;
    anchor = onScalp.position;
    return true;
}

float BrushTool::worldRadiusAt(const ToolView& view, const Vec3f& center, float x, float y) const {
    // The brush circle's edge at the depth of the center: the ring the hosts draw.
    const float radius = groomlab::worldRadiusAt(view, center, x, y, std::max(settings.screenRadius, 1.0f));
    return radius > 0.0f ? std::max(radius, 1e-6f) : 1.0f;
}

void BrushTool::stampAt(StrokeHost& host, const ToolView& view, float x, float y, bool first) {
    Vec3f origin, dir;
    if (!view.ray(x, y, origin, dir)) return;
    using Tool = BrushToolSettings::Tool;
    if (settings.tool == Tool::Brush && settings.paintsScalp()) {
        if (settings.stampsAlpha()) {
            stampAlpha(host, view, x, y);
            return;
        }
        // Scalp maps: painted where the cursor is over the scalp, stamp by stamp; off it, nothing.
        SurfacePoint hit;
        if (!scalp_->intersectRay(origin, dir, &hit)) return;
        hitScalp_ = true;
        stampMap(host, view, hit.position, x, y);
        return;
    }

    Vec3f center;
    if (first && settings.tool == Tool::Brush) {
        // Brushes start on hair or on the scalp; until then, keep looking as the cursor moves.
        Vec3f anchor;
        if (!anchorAt(view, x, y, origin, dir, anchor)) return;
        // At the anchor's depth but under the cursor: the anchor can be up to a brush radius
        // away on screen, and starting there made the first drag jump.
        center = origin + dir * (anchor - origin).dot(dir);
        hitScalp_ = true;
        // Drag in the plane through the starting point, facing the camera.
        planePoint_ = center;
        planeNormal_ = dir;
        last_ = center;
    } else if (first) {
        SurfacePoint hit;
        if (!scalp_->intersectRay(origin, dir, &hit)) return;
        hitScalp_ = true;
        planePoint_ = hit.position;
        planeNormal_ = dir;
        if (settings.tool == Tool::Place) {
            if (placeWithSymmetry(*working_, *scalp_, settings, hit, false) > 0) changed_ = true;
            return;
        }
        if (settings.tool == Tool::Draw) base_ = std::make_shared<GuideHierarchy>(*working_);
        drawPath_ = {hit.position};  // Draw, Part and Flow
        if (settings.tool == Tool::Part && inverted_) stampOverlays(view, hit.position, {}, x, y);  // Ctrl: erase parts
        return;
    } else {
        if (!hitScalp_) return;
        if (settings.tool == Tool::Part || settings.tool == Tool::Flow) {
            // Freehand on the scalp: every point under the cursor, projected onto it.
            SurfacePoint hit;
            if (scalp_->intersectRay(origin, dir, &hit) && (hit.position - drawPath_.back()).length() > 1e-4f) {
                drawPath_.push_back(hit.position);
                if (settings.tool == Tool::Part && inverted_) stampOverlays(view, hit.position, {}, x, y);
            }
            return;
        }
        if (settings.tool == Tool::Place) {
            // Place Many: keep placing under the cursor, spaced.
            SurfacePoint hit;
            if (scalp_->intersectRay(origin, dir, &hit) && placeWithSymmetry(*working_, *scalp_, settings, hit, true) > 0) {
                changed_ = true;
            }
            return;
        }
        const float denom = dir.dot(planeNormal_);
        if (std::fabs(denom) < 1e-6f) return;
        center = origin + dir * ((planePoint_ - origin).dot(planeNormal_) / denom);
        if (settings.tool == Tool::Draw) {
            if ((center - drawPath_.back()).length() > 1e-5f) {
                drawPath_.push_back(center);
                rebuildDrawn();
            }
            return;
        }
    }

    const Vec3f drag = first ? Vec3f{} : center - last_;
    // Guides, unless the brush is set to leave them (erasing or shaping parts and arrows only).
    const bool guides = settings.brush.mode == BrushMode::Erase ? settings.eraseGuides : settings.brushGuides;
    if (guides) {
        // Guides out of the circle's reach skipped; the guides' edits made in parallel when the
        // weight reads the view's matrix (thread-safe), as brushReach says.
        const BrushReachFn reach = view.brushReach(x, y, std::max(settings.screenRadius, 1.0f));
        BrushSettings b = pressed();
        if (b.mirrorTopology) b.radius = worldRadiusAt(view, center, x, y);  // the partner stamp's sphere: the ring's
        if (applyBrushStamp(*working_, scalp_.get(), b, center, drag, screenWeight(view, x, y), reach, &strokeCache_,
                            static_cast<bool>(reach)) > 0) {
            changed_ = true;
        }
    }
    stampOverlays(view, center, drag, x, y);
    last_ = center;
}

void BrushTool::stampOverlays(const ToolView& view, const Vec3f& center, const Vec3f& drag, float x, float y) {
    if (!workingParts_ && !workingFlow_) return;
    BrushSettings b = pressed();
    if (b.mirrorTopology) b.radius = worldRadiusAt(view, center, x, y);  // the partner stamp's sphere: the ring's
    if (settings.tool == BrushToolSettings::Tool::Part) {  // the Part tool with Ctrl: erase, whatever the brush was
        b.mode = BrushMode::Erase;
        b.strength = 1.0f;
    }
    const BrushWeightFn weight = screenWeight(view, x, y);
    if (workingParts_ && applyBrushStampToParts(*workingParts_, *scalp_, b, center, drag, weight) > 0) {
        partsChanged_ = overlayStamped_ = true;
    }
    if (workingFlow_ && applyBrushStampToFlow(*workingFlow_, *scalp_, b, center, drag, weight) > 0) {
        flowChanged_ = overlayStamped_ = true;
    }
}

bool BrushTool::begin(StrokeHost& host, const ToolView& view, float x, float y, float pressure, unsigned modifiers) {
    inverted_ = (modifiers & kStrokeCtrl) != 0;
    pressure_ = pressure;
    return startStroke(host, view, x, y);
}

bool BrushTool::startStroke(StrokeHost& host, const ToolView& view, float x, float y) {
    if (!host.prepareStroke()) return false;
    scalp_ = host.scalp();
    if (!scalp_ || scalp_->triangleCount() == 0) {
        host.warn("GroomLab brush: the groom has no scalp mesh");
        return false;
    }
    overlays_ = OverlayBinding::of(host, scalp_);
    visibility(scalp_, view);  // kept while the camera and the scalp hold still (hovering shares it)
    strokeCache_.clear();
    // Shot sculpting: brushes reshape the groom as it is at this frame (posed and corrected) and
    // the stroke becomes a corrective; guides are neither added nor removed.
    using Tool = BrushToolSettings::Tool;
    shot_ = host.shotSculpt();
    // A Sculpt node targeted: shaping brushes reshape its guides (the node's output) and the
    // stroke becomes the node's edits.
    sculpt_ = false;
    const std::string sculptNode = host.sculptNode();
    if (!sculptNode.empty()) {
        const std::string refusal = sculptRefusal(settings);
        if (!refusal.empty()) {
            host.warn(refusal);
            return false;
        }
        try {
            sculptIn_ = host.sculptInput(sculptNode);
            if (std::abs(sculptIn_.strength) < 1e-3f) throw std::invalid_argument("its strength is 0: raise it to sculpt");
        } catch (const std::exception& e) {
            host.warn("GroomLab sculpt: " + sculptNode + ": " + e.what());
            return false;
        }
        sculptEdits_ = sculptIn_.edits;
        sculpt_ = true;
        shot_ = false;
    }
    if (shot_ && (settings.tool != Tool::Brush || settings.brush.mode == BrushMode::Erase ||
                  settings.brush.mode == BrushMode::Attribute)) {
        host.warn("GroomLab corrective: only shaping brushes record into a corrective (pick the Base groom in "
                  "Sculpt > Correctives to place, draw, erase, paint or part)");
        return false;
    }
    // A scalp map (a vertex or texture map), painted where the brush hits the scalp: the guides,
    // parts and arrows are left alone, so no working copies of them and no preview of them. The
    // host is told a stroke is live (a vertex map, previewed through evaluation, evaluates at the
    // sculpt density).
    mapStroke_ = settings.tool == Tool::Brush && settings.paintsScalp();
    if (mapStroke_) {
        working_.reset();
        workingParts_.reset();
        workingFlow_.reset();
        partsChanged_ = flowChanged_ = overlayStamped_ = false;
        if (!beginMapStroke(host)) {
            mapStroke_ = false;
            return false;
        }
        stroking_ = true;
        changed_ = false;
        hitScalp_ = false;
        alphaStarted_ = alphaPending_ = false;
        ++alphaStroke_;
        alphaDabs_ = 0;
        host.liveStroke(true);
        stampAt(host, view, x, y, true);
        return true;
    }
    // Guide strokes edit the stored (authored) hierarchy, never the posed one: a stroke over a
    // simulated or cached pose must not bake that pose into the guides. The preview then shows
    // the poses and correctives on top. Shot strokes reshape the groom as shown, posed and
    // corrected, and record the difference.
    // A Sculpt node's stroke shapes the node's output now (its input with its stored edits).
    if (sculpt_) {
        const GuidesPtr output = applySculpts(sculptIn_.guides, sculptIn_.edits, sculptIn_.strength, sculptIn_.scalp.get());
        working_ = groomlab::sculptHierarchy(*output, sculptNode, scalp_.get());
    } else {
        const std::shared_ptr<const GuideHierarchy> current = shot_ ? host.shownHierarchy() : host.authoredHierarchy();
        working_ = std::make_shared<GuideHierarchy>(current ? *current : GuideHierarchy{});
    }
    settings.prepareMask(*working_, sculpt_ ? std::set<std::string>{} : host.hiddenGroups(),
                         settings.selectedOnly ? host.selectedGuides() : std::vector<GuideId>{},
                         settings.selectedOnly ? host.softSelectRadius() : 0.0f);
    // Parts and flow arrows the stroke shapes (a brush set to them) or erases (the Erase brush;
    // the Part tool with Ctrl): working copies of the stored data, previewed live and committed
    // with the guides. Not while shot sculpting, nor on a Sculpt node.
    workingParts_.reset();
    workingFlow_.reset();
    partsChanged_ = flowChanged_ = overlayStamped_ = false;
    if (!shot_ && !sculpt_) {
        const bool erasing = settings.tool == Tool::Brush && settings.brush.mode == BrushMode::Erase;
        const bool parts = (settings.tool == Tool::Brush && (erasing ? settings.eraseParts : settings.brushParts)) ||
                           (settings.tool == Tool::Part && inverted_);
        const bool flow = settings.tool == Tool::Brush && (erasing ? settings.eraseFlow : settings.brushFlow);
        try {
            // On the scalp shown (refined when it is subdivided), as the brush meets them.
            if (parts) workingParts_ = std::make_shared<PartSet>(overlays_.shown(PartSet::fromJson(host.storedText(StrokeField::Parts))));
            if (flow) workingFlow_ = std::make_shared<FlowSet>(overlays_.shown(FlowSet::fromJson(host.storedText(StrokeField::Flow))));
        } catch (const std::exception& e) {
            host.warn(std::string("GroomLab brush: ") + e.what());  // bad stored data: brush the guides only
            workingParts_.reset();
            workingFlow_.reset();
        }
        if (workingParts_ && workingParts_->empty()) workingParts_.reset();
        if (workingFlow_ && workingFlow_->empty()) workingFlow_.reset();
    }
    placedBefore_.clear();
    if ((settings.tool == Tool::Place || settings.tool == Tool::Draw) && settings.place.soloNewGuides) {
        for (const HierarchyGuide& g : working_->guides()) placedBefore_.insert(g.id);
    }
    stroking_ = true;
    changed_ = false;
    hitScalp_ = false;
    if (sculpt_) host.liveStroke(true);  // evaluated as any live stroke (its density, its document)
    stampAt(host, view, x, y, true);
    host.previewOverlays(workingParts_, workingFlow_);
    previewGuides(host);
    return true;
}

void BrushTool::previewGuides(StrokeHost& host) {
    if (!sculpt_) {
        host.previewHierarchy(working_, shot_);
        return;
    }
    // The node's edits as this stroke leaves them: the guides it moved recorded again.
    try {
        sculptEdits_ = sculptIn_.edits;
        recordSculptStroke(sculptEdits_, *sculptIn_.guides, *working_, sculptIn_.strength, sculptIn_.scalp.get());
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab sculpt: ") + e.what());
        return;
    }
    host.previewSculpt(sculptIn_.node, std::make_shared<const SculptEdits>(sculptEdits_), working_);
}

void BrushTool::commitSculpt(StrokeHost& host) {
    host.previewSculpt(sculptIn_.node, nullptr, nullptr);  // the stored edits take over
    host.liveStroke(false);
    if (!changed_ || !working_) return;
    try {
        sculptEdits_ = sculptIn_.edits;
        recordSculptStroke(sculptEdits_, *sculptIn_.guides, *working_, sculptIn_.strength, sculptIn_.scalp.get());
        if (sculptEdits_ == sculptIn_.edits) return;  // nothing moved enough to record
        SculptSet set = SculptSet::fromJson(host.storedText(StrokeField::Sculpts));
        if (sculptEdits_.empty()) {
            set.nodes.erase(sculptIn_.node);
        } else {
            set.nodes[sculptIn_.node] = sculptEdits_;
        }
        StrokeEdit edit;
        edit.mode = settings.modeName();
        edit.set(StrokeField::Sculpts, set.toJson());
        host.commit(std::move(edit));
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab sculpt: ") + e.what());
    }
}

ParamMapBinding paintParamMap(StrokeHost& host, BrushToolSettings& settings, const std::string& node, const std::string& param) {
    MeshPtr cage = host.paintScalp();
    if (!cage || cage->triangleCount() == 0) cage = host.scalp();
    const std::shared_ptr<const GuideHierarchy> guides = host.authoredHierarchy();
    ParamMapBinding b = bindParamMap(host.storedText(StrokeField::Graph), host.scalpMaps(), guides.get(),
                                     cage ? cage->pointCount() : 0, node, param);
    if (!b.graphJson.empty()) {  // made and wired: one undo step
        StrokeEdit edit;
        edit.mode = "map " + node + "." + param;
        edit.set(StrokeField::Graph, b.graphJson);
        edit.set(StrokeField::ScalpMaps, b.maps.toJson());
        host.commit(std::move(edit));
    }
    settings.setMode("paint");
    settings.brush.attribute = b.map;
    settings.paintKind = b.kind;
    settings.brush.value = 1.0f;  // strokes paint the param back to its value; Ctrl toward 0
    return b;
}

bool BrushTool::beginMapStroke(StrokeHost& host) {
    paintMapName_ = settings.brush.attribute;
    mapChanged_ = false;
    workingVertexMap_.reset();
    startingVertexMap_.reset();
    paint_.reset();
    if (paintMapName_.empty()) {
        host.warn("GroomLab paint: pick a map to paint (Groom Tools > Paint)");
        return false;
    }
    workingChannels_ = scalpMapChannels(host.scalpMaps(), paintMapName_);
    if (settings.paintKind == "vertices") {
        // Painted per vertex of the cage (the scalp before it is refined: what the map is stored by).
        paintScalp_ = host.paintScalp();
        if (!paintScalp_ || paintScalp_->triangleCount() == 0) paintScalp_ = scalp_;
        bool restarted = false;
        std::vector<float> values = vertexMapToPaint(host.scalpMaps(), paintMapName_, workingChannels_, paintScalp_->pointCount(), &restarted);
        if (restarted) {
            host.warn("GroomLab paint: map " + paintMapName_ +
                      " was painted on a scalp with another vertex count; painting starts it over here");
        }
        workingVertexMap_ = std::make_shared<std::vector<float>>(std::move(values));
        startingVertexMap_ = std::make_shared<const std::vector<float>>(*workingVertexMap_);
        host.dropTexturePaint();  // a texture map is opened again from its file next time
        if (settings.brush.mode == BrushMode::Smooth && neighborsScalp_ != paintScalp_->contentHash()) {
            neighbors_ = VertexNeighbors::of(*paintScalp_);
            neighborsScalp_ = paintScalp_->contentHash();
        }
        return true;
    }
    try {
        paint_ = host.texturePaint(paintMapName_, workingChannels_, settings.paintResolution);
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab paint: ") + e.what());
        return false;
    }
    if (!paint_) return false;
    if (paint_->editing()) paint_->applyEdit(paint_->endEdit(), false);  // a stroke left open: its changes go
    paint_->beginEdit();  // the stroke's tiles, for its undo
    lastTexturePreview_ = {};
    return true;
}

void BrushTool::stampAlpha(StrokeHost& host, const ToolView& view, float x, float y) {
    // Dabs a set distance apart along the cursor's path (each event may make several, or none):
    // an alpha's pattern stamped every alphaSpacing diameters, not smeared. Along the stroke, the
    // first dab waits until the stroke has a direction (a click alone stamps on release).
    const float step = std::max(1.0f, settings.alphaSpacing * 2.0f * std::max(settings.screenRadius, 1.0f));
    if (!alphaStarted_) {
        alphaStarted_ = true;
        alphaX_ = x;
        alphaY_ = y;
        alphaPending_ = settings.alphaRotation == BrushToolSettings::AlphaRotation::Stroke;
        if (!alphaPending_) alphaDab(host, view, x, y, 0.0f);
        return;
    }
    const float dx = x - alphaX_, dy = y - alphaY_;
    float distance = pm::hypot(dx, dy);
    if (distance < step) return;
    const float direction = pm::atan2(dy, dx);
    if (alphaPending_) {
        alphaPending_ = false;
        alphaDab(host, view, alphaX_, alphaY_, direction);
    }
    const float ux = dx / distance, uy = dy / distance;
    for (; distance >= step; distance -= step) {
        alphaX_ += ux * step;
        alphaY_ += uy * step;
        alphaDab(host, view, alphaX_, alphaY_, direction);
    }
}

void BrushTool::finishAlpha(StrokeHost& host, const ToolView& view) {
    if (!alphaPending_) return;
    alphaPending_ = false;  // a click along the stroke: its one dab, at the alpha's own angle
    alphaDab(host, view, alphaX_, alphaY_, 0.0f);
}

void BrushTool::alphaDab(StrokeHost& host, const ToolView& view, float x, float y, float direction) {
    using Rotation = BrushToolSettings::AlphaRotation;
    const std::uint64_t index = alphaDabs_++;
    Vec3f origin, dir;
    SurfacePoint hit;
    if (!view.ray(x, y, origin, dir) || !scalp_->intersectRay(origin, dir, &hit)) return;  // off the scalp: none
    hitScalp_ = true;
    constexpr float kTurn = 6.28318530717958647692f;
    float angle = settings.alphaAngle * (kTurn / 360.0f);
    if (settings.alphaRotation == Rotation::Stroke) angle += direction;
    if (settings.alphaRotation == Rotation::Random) angle = kTurn * unitFloat(hashCombine(hash64(alphaStroke_), index));
    stampMap(host, view, hit.position, x, y, AlphaStamp{settings.alpha, angle, settings.alphaInvert});
}

void BrushTool::stampMap(StrokeHost& host, const ToolView& view, const Vec3f& center, float x, float y, AlphaStamp alpha) {
    const BrushSettings b = pressed();
    const float radius = worldRadiusAt(view, center, x, y);
    const bool stamped = static_cast<bool>(alpha);
    const BrushWeightFn weight = stamped ? view.brushWeight(x, y, std::max(settings.screenRadius, 1.0f), falloffOf(settings.brush), visibility_,
                                                            std::move(alpha))
                                         : screenWeight(view, x, y);
    // Smoothing (Shift+drag, or the Smooth brush on a map): the smooth strength, the pen's
    // pressure on top, in passes past 1. A color map: toward the paint color (Ctrl: its inverse)
    // in the chosen channels; gray: the value.
    MapDab dab = MapDab::of(settings, b, workingChannels_, inverted_, settings.pressure ? pressure_ : 1.0f);
    dab.mirrorWeight = stamped;  // the mirrored dab stamps the alpha mirrored
    MapCanvas canvas;
    canvas.channels = workingChannels_;
    if (workingVertexMap_) {
        canvas.vertices = workingVertexMap_.get();
        canvas.strokeStartVertices = startingVertexMap_.get();
        canvas.neighbors = &neighbors_;
    } else if (paint_) {
        canvas.texture = paint_.get();
    }
    // The screen weight reading the view's matrix may weigh texels in parallel.
    const ScalpMesh& surface = workingVertexMap_ && paintScalp_ ? *paintScalp_ : *scalp_;
    if (paintMapDab(canvas, surface, dab, center, radius, weight, view.parallelWeights()) == 0) return;
    mapChanged_ = true;
    if (workingVertexMap_) {
        // Through evaluation: every param the map feeds follows the brush.
        host.previewVertexMap(paintMapName_, std::make_shared<const std::vector<float>>(*workingVertexMap_), workingChannels_);
    } else if (paint_) {
        // The image in the viewport, a few times a second (the texels the dabs changed since); the
        // groom reads the map when the stroke ends.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTexturePreview_ >= std::chrono::milliseconds(60)) {
            lastTexturePreview_ = now;
            updateTexturePreview(host);
        }
    }
}

void BrushTool::updateTexturePreview(StrokeHost& host) {
    if (!paint_) return;
    if (!preview_) preview_ = std::make_shared<TexturePreview>();
    // As the scalp shows the map: a color map's color, or the channel it shows as gray.
    MapDisplay display;
    display.mapChannels = workingChannels_;
    if (workingChannels_ == kColorMap) {
        const std::string channel = host.scalpMapChannel();
        display.pick = channel == "r" ? 0 : channel == "g" ? 1 : channel == "b" ? 2 : channel == "a" ? 3 : -1;
    }
    preview_->update(*paint_, display);
    host.previewTextureMap(paintMapName_, preview_);
}

void BrushTool::commitMap(StrokeHost& host) {
    if (!mapChanged_) {
        dropMapStroke(&host);
        return;
    }
    try {
        StrokeEdit edit;
        edit.mode = settings.modeName();
        if (workingVertexMap_) {
            edit.set(StrokeField::ScalpMaps, withVertexMap(host.scalpMaps(), paintMapName_, *workingVertexMap_, workingChannels_).toJson());
            host.commit(std::move(edit));  // the stored map is the painted one before the preview clears
            host.previewVertexMap("", nullptr, kGrayMap);
        } else if (paint_) {
            edit.textureMap = paintMapName_;
            edit.texture = paint_->endEdit();
            host.previewTextureMap("", nullptr);
            host.commit(std::move(edit));
        }
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab paint: ") + e.what());
        dropMapStroke(&host);
        return;
    }
    workingVertexMap_.reset();
    paint_.reset();
    mapChanged_ = false;
}

void BrushTool::dropMapStroke(StrokeHost* host) {
    if (host) {
        if (workingVertexMap_) host->previewVertexMap("", nullptr, kGrayMap);
        if (paint_) host->previewTextureMap("", nullptr);
    }
    workingVertexMap_.reset();
    if (paint_ && paint_->editing()) {
        // The stroke's changes go (the paint is as its last stroke left it, for their undo); the
        // file is opened again next time.
        const TextureEdit edit = paint_->endEdit();
        paint_->applyEdit(edit, false);
    }
    paint_.reset();
    if (host) host->dropTexturePaint();
    mapChanged_ = false;
}

void BrushTool::moveTo(StrokeHost& host, const ToolView& view, float x, float y, float pressure) {
    pressure_ = pressure;
    if (!stroking_) return;
    // A stroke pressed over empty space starts where the cursor first reaches hair or scalp.
    const bool changedBefore = changed_;
    changed_ = false;
    overlayStamped_ = false;
    stampAt(host, view, x, y, !hitScalp_);
    const bool stamped = changed_ || overlayStamped_;
    changed_ = changed_ || changedBefore;
    // Only a stamp that moved guides, parts or arrows re-evaluates the groom (a part or flow
    // stroke being drawn is drawn by the host until it is committed).
    if (stamped) previewGuides(host);
}

void BrushTool::commitPart(StrokeHost& host) {
    if (drawPath_.size() < 2 || !scalp_) return;
    try {
        PartSet set = overlays_.shown(PartSet::fromJson(host.storedText(StrokeField::Parts)));
        addPartWithSymmetry(set, *scalp_, settings, drawPath_);
        StrokeEdit edit;
        edit.mode = settings.modeName();
        edit.set(StrokeField::Parts, overlays_.stored(set).toJson());
        host.commit(std::move(edit));
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab part: ") + e.what());  // a click, not a stroke
    }
}

void BrushTool::commitFlow(StrokeHost& host, const ToolView& view) {
    if (drawPath_.empty() || !scalp_) return;
    // Screen-space weights, as the brush circle shows; arrows every half a radius along the stroke.
    std::vector<std::pair<float, float>> screen;
    for (const Vec3f& p : drawPath_) {
        float sx = 0.0f, sy = 0.0f;
        view.project(p, sx, sy);
        screen.emplace_back(sx, sy);
    }
    const float radius = std::max(settings.screenRadius, 1.0f);
    const FlowWeightFn weight = [&](const Vec3f& pin, std::size_t i) {
        float px = 0.0f, py = 0.0f;
        if (!view.project(pin, px, py)) return 0.0f;
        const float dx = px - screen[i].first, dy = py - screen[i].second;
        const float d = std::sqrt(dx * dx + dy * dy) / radius;
        return d >= 1.0f ? 0.0f : (1.0f - d * d) * (1.0f - d * d);
    };
    // World size of half the radius at the stroke's start (the ray through that whole pixel).
    float spacing = 0.0f;
    Vec3f origin, dir;
    const auto [sx, sy] = screen.front();
    if (view.ray(std::trunc(sx + 0.5f * radius), std::trunc(sy), origin, dir)) {
        const Vec3f& p = drawPath_.front();
        const Vec3f toP = p - origin;
        spacing = (origin + dir * toP.dot(dir) - p).length();
    }
    try {
        // Sinks: a click places one the size of the brush there (Ctrl: removes those under it).
        FlowSet set = overlays_.shown(FlowSet::fromJson(host.storedText(StrokeField::Flow)));
        if (settings.flowSink.empty()) {
            flowAlongStroke(set, *scalp_, settings, drawPath_, weight, spacing, inverted_);
        } else {
            flowSinksAt(set, *scalp_, settings, {drawPath_.front()}, 2.0f * spacing, inverted_);
        }
        StrokeEdit edit;
        edit.mode = settings.modeName();
        edit.set(StrokeField::Flow, overlays_.stored(set).toJson());
        host.commit(std::move(edit));
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab flow: ") + e.what());
    }
}

void BrushTool::commitShot(StrokeHost& host) {
    host.previewHierarchy(nullptr, false);  // evaluate without the stroke to get the pose
    const std::shared_ptr<const GuideHierarchy> posed = host.posedHierarchy();
    if (!posed) return;
    const std::string storedLayer = host.storedText(StrokeField::ShotLayer);
    const double seconds = host.seconds();
    const std::string layer = storedLayer.empty() ? "shot" + std::to_string(host.frame()) : storedLayer;
    try {
        Correctives correctives = Correctives::fromJson(host.storedText(StrokeField::Correctives));
        // Recorded against the weights as shown (keyable weights now), then stored as they were.
        const Correctives stored = correctives;
        if (host.usesLiveCorrectiveWeights()) {
            useLiveWeights(correctives, [&](int slot) { return host.liveCorrectiveWeight(slot); });
        }
        correctives.record(layer, *posed, *working_, seconds, scalp_.get());
        for (std::size_t i = 0; i < stored.layers.size() && i < correctives.layers.size(); ++i) {
            CorrectiveLayer& l = correctives.layers[i];
            if (l.slot < 0) continue;
            l.weight = stored.layers[i].weight;
            l.holdBefore = stored.layers[i].holdBefore;
            l.holdAfter = stored.layers[i].holdAfter;
            l.weightCurve = stored.layers[i].weightCurve;
        }
        StrokeEdit edit;
        edit.mode = settings.modeName();
        edit.set(StrokeField::Correctives, correctives.toJson());
        // The first stroke names the layer: part of the same undo step, so undoing it clears it.
        if (storedLayer.empty()) edit.set(StrokeField::ShotLayer, layer);
        host.commit(std::move(edit));
    } catch (const std::exception& e) {
        host.warn(std::string("GroomLab shot sculpt: ") + e.what());
    }
}

void BrushTool::end(StrokeHost& host, const ToolView& view) {
    stroking_ = false;
    strokeCache_.clear();
    using Tool = BrushToolSettings::Tool;
    if (settings.tool == Tool::Part && !inverted_) commitPart(host);
    if (settings.tool == Tool::Flow) commitFlow(host, view);
    if (settings.tool == Tool::Brush && settings.paintsScalp()) {
        finishAlpha(host, view);
        commitMap(host);
    }
    base_.reset();
    drawPath_.clear();
    if (mapStroke_) {
        // A scalp map stroke: committed above; no guides, parts or arrows were previewed.
        mapStroke_ = false;
        working_.reset();
        host.liveStroke(false);
        return;
    }
    if (sculpt_) {
        commitSculpt(host);
        sculpt_ = false;
        working_.reset();
        sculptIn_ = SculptInput{};
        return;
    }
    const bool guides = changed_ && working_ && !shot_;
    if (guides && (settings.tool == Tool::Place || settings.tool == Tool::Draw) && settings.place.soloNewGuides) {
        // Solo new guides: the stroke's new guides alone stay unfrozen, in its undo step.
        std::vector<GuideId> made;
        for (const HierarchyGuide& g : working_->guides()) {
            if (!placedBefore_.count(g.id)) made.push_back(g.id);
        }
        if (!made.empty()) soloGuides(*working_, made, settings.brush.freeze.empty() ? std::string("freeze") : settings.brush.freeze);
    }
    placedBefore_.clear();
    if (changed_ && working_ && shot_) commitShot(host);
    // The parts and arrows the stroke shaped or erased (its working copies).
    const bool parts = partsChanged_ && workingParts_;
    const bool flow = flowChanged_ && workingFlow_;
    if (guides || parts || flow) {
        StrokeEdit edit;  // one undo step for all of it
        edit.mode = settings.modeName();
        if (guides) edit.set(StrokeField::Guides, working_->toJson());
        if (parts) edit.set(StrokeField::Parts, overlays_.stored(*workingParts_).toJson());  // back on the cage
        if (flow) edit.set(StrokeField::Flow, overlays_.stored(*workingFlow_).toJson());
        host.previewOverlays(nullptr, nullptr);  // the stored data takes over
        host.previewHierarchy(nullptr, false);
        host.commit(std::move(edit));
    } else {
        host.previewOverlays(nullptr, nullptr);
        host.previewHierarchy(nullptr, false);
    }
    working_.reset();
    workingParts_.reset();
    workingFlow_.reset();
}

void BrushTool::cancel(StrokeHost* host) {
    stroking_ = false;
    if (sculpt_ && host) host->previewSculpt(sculptIn_.node, nullptr, nullptr);
    sculpt_ = false;
    sculptIn_ = SculptInput{};
    base_.reset();
    working_.reset();
    workingParts_.reset();
    workingFlow_.reset();
    drawPath_.clear();
    dropMapStroke(host);
    mapStroke_ = false;
    if (host) {
        host->liveStroke(false);
        host->previewOverlays(nullptr, nullptr);
        host->previewHierarchy(nullptr, false);
    }
}

void BrushTool::press(StrokeHost& host, const ToolView& view, float x, float y, float pressure, unsigned modifiers) {
    if (settings.tool == BrushToolSettings::Tool::Lengths) {
        lengthsPressed_ = host.attach() && lengths_.press(host, view, settings, x, y, modifiers);
        return;
    }
    inverted_ = (modifiers & kStrokeCtrl) != 0;
    pressure_ = pressure;
    if (modifiers & kStrokeShift) {
        // Painting, a drag smooths (decided on drag); a release without one does nothing. A guide
        // brush's Shift selects (the host's): nothing here.
        if (settings.shiftSelects()) return;
        shiftPending_ = true;
        pressX_ = x;
        pressY_ = y;
        return;
    }
    startStroke(host, view, x, y);
}

void BrushTool::drag(StrokeHost& host, const ToolView& view, float x, float y, float pressure) {
    if (lengthsPressed_) {
        lengths_.drag(host, view, settings, x, y);
        return;
    }
    pressure_ = pressure;
    if (shiftPending_) {
        const float moved = pm::hypot(x - pressX_, y - pressY_);
        if (moved > 4.0f && settings.tool == BrushToolSettings::Tool::Brush) {
            shiftPending_ = false;
            shiftSmoothing_ = true;
            savedMode_ = settings.brush.mode;
            useShiftSmoothing(settings.brush);  // Smooth, or the Shape brush smoothing its channel
            if (!startStroke(host, view, pressX_, pressY_)) {
                endShiftSmoothing(settings.brush, savedMode_);
                shiftSmoothing_ = false;
            }
        }
        if (shiftPending_) return;
    }
    if (stroking_) moveTo(host, view, x, y, pressure);
}

StrokeRelease BrushTool::release(StrokeHost& host, const ToolView& view) {
    if (lengthsPressed_) {
        lengthsPressed_ = false;
        lengths_.release(host, settings);
        return StrokeRelease::Stroke;
    }
    if (shiftPending_) {
        shiftPending_ = false;  // a Shift+click: nothing (no drag, no smoothing)
        return StrokeRelease::None;
    }
    const bool stroked = stroking_;
    if (stroking_) end(host, view);
    if (shiftSmoothing_) {
        shiftSmoothing_ = false;
        endShiftSmoothing(settings.brush, savedMode_);
    }
    return stroked ? StrokeRelease::Stroke : StrokeRelease::None;
}

bool BrushTool::stroke(StrokeHost& host, const ToolView& view, const std::vector<std::array<float, 2>>& points, float pressure,
                       unsigned modifiers, const std::function<void()>& afterDab) {
    inverted_ = (modifiers & kStrokeCtrl) != 0;
    pressure_ = pressure;
    if ((modifiers & kStrokeShift) && settings.shiftSelects()) return false;  // a guide brush's Shift selects: the host's
    const BrushMode saved = settings.brush.mode;
    if (modifiers & kStrokeShift) useShiftSmoothing(settings.brush);  // as a Shift+drag while painting does
    if (points.empty() || !startStroke(host, view, points[0][0], points[0][1])) {
        endShiftSmoothing(settings.brush, saved);
        return false;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        moveTo(host, view, points[i][0], points[i][1], pressure);
        if (afterDab) afterDab();
    }
    if (mapStroke_) finishAlpha(host, view);
    const bool result = changed_ || mapChanged_;
    end(host, view);
    endShiftSmoothing(settings.brush, saved);
    return result;
}

int BrushTool::pickLevel(StrokeHost& host, const ToolView& view, float x, float y) {
    if (!host.attach()) return -2;
    const std::shared_ptr<const GuideHierarchy> h = host.shownHierarchy();
    if (!h) return -2;
    Vec3f origin, dir;
    if (!view.ray(x, y, origin, dir)) return -2;
    GuidePointHit hit;
    if (!nearestGuidePoint(*h, view, x, y, std::clamp(settings.screenRadius, 8.0f, 60.0f), origin, dir, nullptr, &hit)) return -2;
    const int level = h->groupDepth(hit.guide->group);
    const GuideGroup* group = h->group(hit.guide->group);
    settings.brush.level = level;
    settings.brush.group = group ? group->name : std::string();  // brush the picked guide's group
    return level;
}

std::vector<std::vector<Vec3f>> BrushTool::affectedLines(StrokeHost& host, const ToolView& view, float x, float y) {
    // Before pressing: the guides the brush would touch, as drawn (smoothed).
    std::vector<std::vector<Vec3f>> lines;
    if (settings.tool != BrushToolSettings::Tool::Brush || !host.attach()) return lines;
    const std::shared_ptr<const GuideHierarchy> h = host.shownHierarchy();
    if (!h) return lines;
    visibility(settings.maskByScalp ? host.scalp() : nullptr, view);
    const BrushWeightFn weight = screenWeight(view, x, y);
    BrushSettings b = settings.brush;
    const bool sculpting = !host.sculptNode().empty();
    if (sculpting) {  // a Sculpt node's guides: all of them (pressed())
        b.level = 0;
        b.group.clear();
    }
    const int smoothing = host.guideSmoothing();
    const std::set<std::string> hidden = sculpting ? std::set<std::string>{} : host.hiddenGroups();
    auto brushable = [&](const HierarchyGuide& g) {
        if (!hidden.empty()) {
            const GuideGroup* grp = h->group(g.group);
            if (grp && hidden.count(grp->name)) return false;
        }
        if (!b.group.empty()) {
            const GuideGroup* grp = h->group(g.group);
            return grp && grp->name == b.group;
        }
        return b.level < 0 || h->groupDepth(g.group) == b.level;
    };
    auto touched = [&](const HierarchyGuide& g) {
        for (const Vec3f& p : g.points) {
            if (weight(p) > 0.05f) return true;
        }
        return false;
    };
    if (!view.projection.valid()) {
        hover_.reset();
        for (const HierarchyGuide& g : h->guides()) {
            if (lines.size() > 4000) break;  // a hint, not a full redraw
            if (!brushable(g) || !touched(g)) continue;
            lines.push_back(smoothPolyline(g.points.data(), g.points.size(), smoothing));
        }
        return lines;
    }
    // The brushable guides and their points on screen, kept while the guides, what is brushable
    // and the view hold still: a mouse move looks at the guides near the circle only.
    std::uint64_t filter = hashString(hashCombine(hash64(0x40E3), static_cast<std::uint64_t>(static_cast<std::int64_t>(b.level))), b.group);
    for (const std::string& name : hidden) filter = hashString(filter, name);
    const std::uint64_t version = host.shownHierarchyVersion();
    if (!hover_ || hover_->hierarchy != h || hover_->hierarchyVersion != version || hover_->filter != filter) {
        hover_ = std::make_shared<HoverCache>();
        hover_->hierarchy = h;
        hover_->hierarchyVersion = version;
        hover_->filter = filter;
        for (const HierarchyGuide& g : h->guides()) {
            if (brushable(g)) hover_->guides.push_back(&g);
        }
    }
    HoverCache& hover = *hover_;
    if (!hover.grid || hover.view != view.projection.hash()) {
        std::vector<const std::vector<Vec3f>*> points;
        points.reserve(hover.guides.size());
        for (const HierarchyGuide* g : hover.guides) points.push_back(&g->points);
        hover.grid = std::make_unique<ScreenPointGrid>(view.projection, points);
        hover.view = view.projection.hash();
    }
    if (hover.smoothing != smoothing) {
        hover.smoothing = smoothing;
        hover.lines.assign(hover.guides.size(), {});
        hover.made.assign(hover.guides.size(), 0);
    }
    for (const std::uint32_t k : hover.grid->near(x, y, std::max(settings.screenRadius, 1.0f))) {
        if (lines.size() > 4000) break;  // a hint, not a full redraw
        const HierarchyGuide& g = *hover.guides[k];
        if (!touched(g)) continue;
        if (!hover.made[k]) {
            hover.lines[k] = smoothPolyline(g.points.data(), g.points.size(), smoothing);
            hover.made[k] = 1;
        }
        lines.push_back(hover.lines[k]);
    }
    return lines;
}

// ---- Scripted strokes ------------------------------------------------------------------------------

std::size_t scriptedStroke(StrokeHost& host, BrushToolSettings s, const std::vector<Vec3f>& path) {
    using Tool = BrushToolSettings::Tool;
    const MeshPtr scalp = host.scalp();
    auto requireScalp = [&] {
        if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the groom has no scalp mesh");
    };
    StrokeEdit edit;
    edit.mode = s.modeName();
    // A Sculpt node targeted: its guides brushed, its edits committed.
    const std::string sculptNode = host.sculptNode();
    if (!sculptNode.empty()) {
        const std::string refusal = sculptRefusal(s);
        if (!refusal.empty()) throw std::invalid_argument(refusal);
        requireScalp();
        const SculptInput in = host.sculptInput(sculptNode);
        const std::shared_ptr<GuideHierarchy> h =
            sculptHierarchy(*applySculpts(in.guides, in.edits, in.strength, in.scalp.get()), sculptNode, scalp.get());
        s.brush.level = 0;
        s.brush.group.clear();
        s.prepareMask(*h, {}, s.selectedOnly ? host.selectedGuides() : std::vector<GuideId>{},
                      s.selectedOnly ? host.softSelectRadius() : 0.0f);
        applyBrushStroke(*h, scalp.get(), s.brush, path);
        SculptEdits edits = in.edits;
        recordSculptStroke(edits, *in.guides, *h, in.strength, in.scalp.get());
        if (edits == in.edits) return 0;
        SculptSet set = SculptSet::fromJson(host.storedText(StrokeField::Sculpts));
        if (edits.empty()) {
            set.nodes.erase(sculptNode);
        } else {
            set.nodes[sculptNode] = std::move(edits);
        }
        edit.set(StrokeField::Sculpts, set.toJson());
        host.commit(std::move(edit));
        return 0;
    }
    const ScalpMaps& storedMaps = host.scalpMaps();
    if (s.tool == Tool::Brush && s.paintsScalp()) {
        requireScalp();
        const float radius = std::max(s.brush.radius, 1e-6f);
        const std::string name = s.brush.attribute;
        if (name.empty()) throw std::invalid_argument("-attribute names the map to paint");
        const int channels = scalpMapChannels(storedMaps, name);
        const MapDab dab = MapDab::of(s, s.brush, channels, false, std::abs(s.brush.strength));
        std::size_t changed = 0;
        if (s.paintKind == "vertices") {
            // Stored per vertex of the cage: painted on it, as the interactive brush paints.
            MeshPtr cage = host.paintScalp();
            if (!cage || cage->triangleCount() == 0) cage = scalp;
            std::vector<float> values = vertexMapToPaint(storedMaps, name, channels, cage->pointCount());
            const VertexNeighbors neighbors = dab.smoothing ? VertexNeighbors::of(*cage) : VertexNeighbors{};
            MapCanvas canvas;
            canvas.vertices = &values;
            canvas.neighbors = &neighbors;
            canvas.channels = channels;
            changed = paintMapStroke(canvas, *cage, dab, path, radius);
            edit.set(StrokeField::ScalpMaps, withVertexMap(storedMaps, name, std::move(values), channels).toJson());
        } else {
            const std::shared_ptr<TexturePaint> paint = host.texturePaint(name, channels, s.paintResolution);
            if (!paint) throw std::runtime_error("the groom has no texture map named " + name);
            MapCanvas canvas;
            canvas.texture = paint.get();
            canvas.channels = channels;
            const bool record = !paint->editing();
            if (record) paint->beginEdit();
            changed = paintMapStroke(canvas, *scalp, dab, path, radius);
            if (record) edit.texture = paint->endEdit();
            edit.textureMap = name;
        }
        host.commit(std::move(edit));
        return changed;
    }
    if (s.tool == Tool::Flow) {
        requireScalp();
        std::vector<Vec3f> onScalp;
        for (const Vec3f& p : path) onScalp.push_back(scalp->closestPoint(p).position);
        const float radius = std::max(s.brush.radius, 1e-6f);
        const FlowWeightFn weight = [&](const Vec3f& pin, std::size_t i) {
            const float d = (pin - onScalp[i]).length() / radius;
            return d >= 1.0f ? 0.0f : (1.0f - d * d) * (1.0f - d * d);
        };
        const bool erase = s.brush.strength < 0.0f;
        const OverlayBinding overlays = OverlayBinding::of(host, scalp);
        FlowSet set = overlays.shown(FlowSet::fromJson(host.storedText(StrokeField::Flow)));
        if (s.flowSink.empty()) {
            flowAlongStroke(set, *scalp, s, onScalp, weight, 0.5f * radius, erase);
        } else {
            flowSinksAt(set, *scalp, s, onScalp, radius, erase);
        }
        edit.set(StrokeField::Flow, overlays.stored(set).toJson());
        host.commit(std::move(edit));
        return 0;
    }
    if (s.tool == Tool::Part) {
        requireScalp();
        const OverlayBinding overlays = OverlayBinding::of(host, scalp);
        PartSet set = overlays.shown(PartSet::fromJson(host.storedText(StrokeField::Parts)));
        addPartWithSymmetry(set, *scalp, s, path);
        edit.set(StrokeField::Parts, overlays.stored(set).toJson());
        host.commit(std::move(edit));
        return 0;
    }
    // A brush on the parts and the flow arrows (its targets): the stored data, brushed the same way.
    const bool brush = s.tool == Tool::Brush;
    if (brush && s.brushParts) {
        requireScalp();
        const OverlayBinding overlays = OverlayBinding::of(host, scalp);
        PartSet set = overlays.shown(PartSet::fromJson(host.storedText(StrokeField::Parts)));
        if (applyBrushStrokeToParts(set, *scalp, s.brush, path) > 0) edit.set(StrokeField::Parts, overlays.stored(set).toJson());
    }
    if (brush && s.brushFlow) {
        requireScalp();
        const OverlayBinding overlays = OverlayBinding::of(host, scalp);
        FlowSet set = overlays.shown(FlowSet::fromJson(host.storedText(StrokeField::Flow)));
        if (applyBrushStrokeToFlow(set, *scalp, s.brush, path) > 0) edit.set(StrokeField::Flow, overlays.stored(set).toJson());
    }
    if (!brush || s.brushGuides) {
        const std::shared_ptr<GuideHierarchy> h = host.editableHierarchy();
        s.prepareMask(*h, host.hiddenGroups(), s.selectedOnly ? host.selectedGuides() : std::vector<GuideId>{},
                      s.selectedOnly ? host.softSelectRadius() : 0.0f);
        requireScalp();
        std::set<GuideId> before;
        for (const HierarchyGuide& g : h->guides()) before.insert(g.id);
        if (s.tool == Tool::Place) {
            for (const Vec3f& p : path) placeWithSymmetry(*h, *scalp, s, scalp->closestPoint(p), false);
        } else if (s.tool == Tool::Draw) {
            drawWithSymmetry(*h, *scalp, s, path);
        } else {
            applyBrushStroke(*h, scalp.get(), s.brush, path);
        }
        if ((s.tool == Tool::Place || s.tool == Tool::Draw) && s.place.soloNewGuides) {
            std::vector<GuideId> made;
            for (const HierarchyGuide& g : h->guides()) {
                if (!before.count(g.id)) made.push_back(g.id);
            }
            if (!made.empty()) soloGuides(*h, made, s.brush.freeze.empty() ? std::string("freeze") : s.brush.freeze);
        }
        edit.set(StrokeField::Guides, h->toJson());
    }
    host.commit(std::move(edit));
    return 0;
}

}  // namespace groomlab
