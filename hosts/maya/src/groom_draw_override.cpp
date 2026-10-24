// SPDX-License-Identifier: Apache-2.0
// Viewport 2.0 drawing for groomLabShape (groom_draw_override.h): the override's life, its update,
// the UI drawables and the report-only timings. The other parts are in groom_draw_override_*.cpp.

#include "groom_draw_override_internal.h"
#include "gpu_lines.h"
#include "gpu_preview.h"
#include "hair_shading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <maya/MColorArray.h>
#include <maya/MPointArray.h>
#include <maya/MUintArray.h>
#include <maya/MVector.h>
#include <maya/MUIDrawManager.h>
#include <maya/MFrameContext.h>
#include <maya/MHWGeometryUtilities.h>
#include <maya/MGlobal.h>
#include <maya/MDagPath.h>
#include <maya/MDagPathArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MHWGeometry.h>
#include <maya/MMessage.h>
#include <maya/MPlug.h>
#include <maya/MShaderManager.h>
#include <maya/MTextureManager.h>
#include <maya/MViewport2Renderer.h>

#include "display_theme.h"
#include "groom_shape.h"
#include "maya_inputs.h"
#include "groomlab/cards.h"
#include "groomlab/color_patterns.h"
#include "groomlab/display.h"
#include "groomlab/hair_color.h"
#include "groomlab/instances.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/texture.h"

namespace groomlab::maya {

// The draw overrides drawing each groom (test hooks: gpuFramesCheckJson, gpuFramesSwitched).
std::multimap<const GroomShape*, GroomSubSceneOverride*>& drawnOverrides() {
    static std::multimap<const GroomShape*, GroomSubSceneOverride*> all;
    return all;
}

namespace {

unsigned depthPriority(DisplayLayer layer) {
    switch (layer) {
        case DisplayLayer::DormantFilled: return MHWRender::MRenderItem::sDormantFilledDepthPriority;
        case DisplayLayer::ActiveWire: return MHWRender::MRenderItem::sActiveWireDepthPriority;
        case DisplayLayer::Selection: return MHWRender::MRenderItem::sSelectionDepthPriority;
        default: return MHWRender::MRenderItem::sDormantWireDepthPriority;
    }
}

}  // namespace

const char* GroomSubSceneOverride::kRegistrantId = "GroomLabSubSceneOverride";

MHWRender::MPxSubSceneOverride* GroomSubSceneOverride::creator(const MObject& obj) {
    return new GroomSubSceneOverride(obj);
}

GroomSubSceneOverride::GroomSubSceneOverride(const MObject& obj)
    : MHWRender::MPxSubSceneOverride(obj), object_(obj) {
    builder_.setRetain(false);  // the buffers live on the GPU; the builder keeps only what they were built for
    builder_.setWriteLines(true);  // the strand lines are written straight into their buffers (updateItem)
    builder_.setRibbonLines(true);  // ribbons as lines: the GPU expands them (writeGpuRibbons), else built at upload
    MFnDependencyNode fn(obj);
    shape_ = dynamic_cast<GroomShape*>(fn.userNode());
    if (shape_) gpuPreview_ = std::make_unique<GpuLookPreview>(shape_);  // nothing made on the GPU until a drag
    gpuLines_ = std::make_unique<GpuLines>();  // its engine made at the first lit lines' write
    if (shape_) drawnOverrides().emplace(shape_, this);
}

GroomSubSceneOverride::~GroomSubSceneOverride() {
    for (auto [at, end] = drawnOverrides().equal_range(shape_); at != end; ++at) {
        if (at->second == this) {
            drawnOverrides().erase(at);
            break;
        }
    }
    MMessage::removeCallbacks(callbacks_);
    MMessage::removeCallbacks(prototypeCallbacks_);
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    const MHWRender::MShaderManager* mgr = renderer ? renderer->getShaderManager() : nullptr;
    if (!mgr) return;
    for (auto& [name, item] : items_) {
        if (item.shader) mgr->releaseShader(item.shader);
    }
    for (MHWRender::MShaderInstance*& shader : lodShaders_) {
        if (shader) mgr->releaseShader(shader);
        shader = nullptr;
    }
    if (rootToTip_) {
        if (MHWRender::MTextureManager* textures = renderer->getTextureManager()) textures->releaseTexture(rootToTip_);
    }
    releaseCardTextures();
    releaseScalpMapTexture();
}

namespace {

// The dots the last viewport update built (any groom's): all of them, and those in the theme's
// clump centers color (a viewed Clump's centers).
struct DotCounts {
    std::size_t dots = 0, clumpCenters = 0;
};
DotCounts& lastDots() {
    static DotCounts d;
    return d;
}

}  // namespace

DrawTimes& currentDraw() {
    static DrawTimes t;
    return t;
}
LodStats& lastLod() {
    static LodStats s;
    return s;
}
DrawTimes& lastDraw() {
    static DrawTimes t;
    return t;
}

// Under OpenGL the items' vertex and index buffers live on the GPU alone: no system-memory copy
// (Maya's default keeps one and uploads it again at the next draw: at 500k strands x 16 points,
// the lit strands' 380 MB took ~50 ms to commit and ~65 ms more in the next draw). Under
// DirectX 11, Maya 2022 gives no memory to write a GPU-only buffer (acquire returns null, every
// time), so there they keep the system-memory copy. Each item keeps its buffers and writes them
// again in place (acquire, write, commit), never makes new ones per update: a new buffer cost a
// few hundred ms in Maya's draw at that size.
bool softwareStaged() {
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    return renderer && renderer->drawAPI() == MHWRender::kDirectX11;
}

std::string lastDrawTimesJson() {
    const DrawTimes& t = lastDraw();
    char buffer[1024];
    std::string out;
    std::snprintf(buffer, sizeof buffer,
                  "{\"serial\": %llu, \"updateMs\": %.3f, \"buildMs\": %.3f, \"uploadMs\": %.3f, \"pullMs\": %.3f, \"previewMs\": %.3f, \"gridMs\": %.3f, \"settingsMs\": %.3f, \"afterMs\": %.3f, \"lodMs\": %.3f, \"facing\": %s, \"items\": {",
                  static_cast<unsigned long long>(t.serial), t.updateMs, t.buildMs, t.uploadMs, t.pullMs, t.previewMs, t.gridMs, t.settingsMs,
                  t.afterMs, t.lodMs, t.facing ? "true" : "false");
    out = buffer;
    bool first = true;
    for (const auto& [name, item] : t.items) {
        std::snprintf(buffer, sizeof buffer,
                      "%s\"%s\": {\"acquireMs\": %.3f, \"writeMs\": %.3f, \"commitMs\": %.3f, \"bindMs\": %.3f, \"megabytes\": %.3f, "
                      "\"partialStrands\": %zu, \"refaced\": %d, \"gpuFrames\": %s, \"gpuParts\": [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f], \"gpuMB\": %.3f, "
                      "\"follow\": %s, \"streams\": \"%s\"}",
                      first ? "" : ", ", name.c_str(), item.acquireMs, item.writeMs, item.commitMs, item.bindMs, item.megabytes,
                      item.partialStrands, item.refaced, item.gpuFrames ? "true" : "false", item.gpuParts[0], item.gpuParts[1], item.gpuParts[2],
                      item.gpuParts[3], item.gpuParts[4], item.gpuParts[5], item.gpuMegabytes, item.follow ? "true" : "false",
                      item.streams.substr(0, 300).c_str());
        out += buffer;
        first = false;
    }
    // Each item's shader as styled this update, and how many items share its instance.
    out += "}, \"shaders\": {";
    first = true;
    for (const auto& [name, s] : t.shaders) {
        std::size_t sharing = 0;
        for (const auto& [otherName, other] : t.shaders) sharing += other.instance == s.instance ? 1 : 0;
        std::snprintf(buffer, sizeof buffer,
                      "%s\"%s\": {\"instance\": \"%llx\", \"kind\": %d, \"hair\": %s, \"coverage\": %s, \"registered\": %d, "
                      "\"lineDensity\": %.4f, \"widthScale\": %.4f, \"shared\": %zu}",
                      first ? "" : ", ", name.c_str(), static_cast<unsigned long long>(s.instance), s.kind, s.hair ? "true" : "false",
                      s.coverage ? "true" : "false", s.registered, s.lineDensity, s.widthScale, sharing);
        out += buffer;
        first = false;
    }
    std::snprintf(buffer, sizeof buffer, "}, \"hairDrawsUnknown\": %lld", hairDrawsUnknown());
    out += buffer;
    // The strand lines' draws with width as coverage since the plugin loaded, by kind (CoverageBlend).
    std::snprintf(buffer, sizeof buffer, ", \"dots\": %zu, \"clumpCenterDots\": %zu, \"coverageDraws\": {\"hashed\": %lld, \"sampled\": %lld, \"blendItem\": %lld}",
                  lastDots().dots, lastDots().clumpCenters, coverageBlend().hashed, coverageBlend().sampled, coverageBlend().blendItemDraws);
    out += buffer;
    // The textured cards' color draws since the plugin loaded: on multisampled targets (their
    // coverage as the samples covered) and on one sample a pixel (the alpha test).
    long long cardsSampled = 0, cardsTested = 0;
    cardCoverageDraws(cardsSampled, cardsTested);
    std::snprintf(buffer, sizeof buffer, ", \"cardDraws\": {\"sampled\": %lld, \"tested\": %lld}", cardsSampled, cardsTested);
    out += buffer;
    // The screen-space level of detail (updateLod).
    const LodStats& lod = lastLod();
    std::snprintf(buffer, sizeof buffer,
                  ", \"lod\": {\"on\": %s, \"level\": %d, \"drawnStrands\": %zu, \"totalStrands\": %zu, \"groups\": %zu, \"clumped\": %s, "
                  "\"thinnedGroups\": %zu, \"impostors\": %zu, \"blendSegments\": %zu, \"items\": %zu, \"ms\": %.3f, \"indicesMs\": %.3f, "
                  "\"megabytes\": %.3f, \"updates\": %lld, \"rebuilds\": %lld, \"selectMs\": %.3f, \"slotsWritten\": %zu, \"farLevel\": %d, "
                  "\"slotsBound\": %lld, \"baseSerial\": %llu, \"staleSlots\": %zu}}",
                  lod.on ? "true" : "false", lod.level == kNoLodLevel ? -9999 : lod.level, lod.drawnStrands, lod.totalStrands, lod.groups,
                  lod.clumped ? "true" : "false", lod.thinnedGroups, lod.impostors, lod.blendSegments, lod.items, lod.ms, lod.indicesMs,
                  lod.megabytes, lod.updates, lod.rebuilds, lod.selectMs, lod.slotsWritten, lod.farLevel == kNoLodLevel ? -9999 : lod.farLevel,
                  lod.slotsBound, static_cast<unsigned long long>(lod.baseSerial), lod.staleSlots);
    return out + buffer;
}

MHWRender::DrawAPI GroomSubSceneOverride::supportedDrawAPIs() const { return MHWRender::kAllDevices; }

bool GroomSubSceneOverride::visible() const {
    MDagPathArray paths;
    if (MDagPath::getAllPathsTo(object_, paths) != MS::kSuccess) return true;
    for (unsigned i = 0; i < paths.length(); ++i) {
        if (paths[i].isVisible()) return true;
    }
    return false;
}

bool GroomSubSceneOverride::displayFlag(const MObject& attr) const { return MPlug(object_, attr).asBool(); }

float GroomSubSceneOverride::floatPlug(const MObject& attr) const { return MPlug(object_, attr).asFloat(); }

bool GroomSubSceneOverride::requiresUpdate(const MHWRender::MSubSceneContainer&,
                                           const MHWRender::MFrameContext& frameContext) const {
    if (!shape_) return false;
    try {
        // The dots are built for one camera and viewport size: another one rebuilds them.
        if (!points_.empty()) {
            int ox = 0, oy = 0, width = 0, height = 0;
            frameContext.getViewportDimensions(ox, oy, width, height);
            const MMatrix camera = frameContext.getMatrix(MHWRender::MFrameContext::kViewProjMtx);
            if (height != uiHeight_ || !camera.isEquivalent(uiCamera_)) {
                uiCamera_ = camera;
                uiHeight_ = height;
                uiDirty_ = true;
            }
        }
        if (!occluders_.empty() && !overlayBatches_.empty() && displayFlag(GroomShape::aDisplayOverlaysOnTop)) {
            const MMatrix view = frameContext.getMatrix(MHWRender::MFrameContext::kViewInverseMtx);
            const Vec3f camera(static_cast<float>(view(3, 0)), static_cast<float>(view(3, 1)), static_cast<float>(view(3, 2)));
            if (!overlayCameraKnown_ || (camera - overlayCamera_).length() > 1e-4f * std::max(overlayPull_, 1e-3f)) return true;
        }
        // Ribbons facing the camera: made again for each new view.
        if (facing_) {
            int ox = 0, oy = 0, width = 0, height = 0;
            frameContext.getViewportDimensions(ox, oy, width, height);
            if (height != facingHeight_ || !frameContext.getMatrix(MHWRender::MFrameContext::kViewProjMtx).isEquivalent(facingCamera_)) {
                return true;
            }
        }
        // The level of detail: another level (the camera came nearer or went farther) draws again;
        // an orbit keeps it.
        if (lodWanted() && lodViewOf(frameContext) != lodDrawn_) return true;
        // Multisample anti-aliasing turned on or off: the blendable segments split, or join, again.
        if (lodWanted() && frameContext.getPostEffectEnabled(MHWRender::MFrameContext::kAntiAliasing) != lodAntiAliased_) return true;
        // The cards' wireframe in the groom's wireframe color: selected or not, recolored.
        if (cardWiresShown_ && wireColor() != cardWireColor_) return true;
        return needsUpdate();
    } catch (const std::exception&) {
        return true;  // update() reports it
    }
}

bool GroomSubSceneOverride::needsUpdate() const {
    const bool shown = visible();
    if (shown != drawnVisible_) return true;
    if (!shown) return false;  // hidden: nothing to evaluate or draw
    shape_->handedStrands();  // pull evaluation so the versions are current (nothing carried)
    // A commit evaluating off the UI thread (groom_shape.h expectCommit): what is drawn (the GPU
    // preview's results) stays until it lands, which marks the drawing dirty.
    if (shape_->commitPending()) return false;
    if (shape_->evaluationVersion() != drawnVersion_ || shape_->displayVersion() != drawnDisplay_ ||
        displayTheme().version != drawnTheme_ || shape_->selectionHash() != drawnSelection_) {
        return true;
    }
    // Card maps rewritten in place (a bake from another session, a paint-over): reload them.
    if (cardFilesChanged()) return true;
    // A GPU preview's values changed (gpu_preview.h).
    if (gpuPreview_) {
        const GpuPreviewRequest* request = gpuPreviewRequest(shape_);
        if (request && request->version != gpuDrawn_) return true;
    }
    // The map shown on the scalp: painted (its preview), rewritten, or another one picked.
    if (shape_->scalpMapView().version != scalpMapVersion_) return true;
    // A prototype mesh edited, replaced or deleted: its instances draw it.
    if (prototypesChanged()) return true;
    // A material's color lives on other nodes: read again once one of them was dirtied.
    // (update() reads it and re-watches the nodes it found; cheap when nothing else changed.)
    return colorDirty_ && MPlug(object_, GroomShape::aUseMaterialColor).asBool();
}

// Guide point dots: round discs facing the camera, a constant size on screen,
// depth tested (dots behind the head stay hidden). Rebuilt when the guides,
// the camera or the viewport changed (uiDirty_); the positions and colors come
// from the last update().
bool GroomSubSceneOverride::hasUIDrawables() const { return true; }

bool GroomSubSceneOverride::areUIDrawablesDirty() const {
    return uiDirty_ || (shape_ && (shape_->symmetrySeamVersion() != seamDrawn_ || shape_->tubeWires() != tubeWiresDrawn_ ||
                                   (tubeWiresDrawn_ && shape_->selectionHash() != uiSelection_)));
}

void GroomSubSceneOverride::addUIDrawables(MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& context) {
    uiDirty_ = false;
    try {
        addDots(draw, context);
        // A brush with symmetry on this groom: the scalp's line of symmetry in red (symmetrySeam),
        // as Substance Painter shows it, whenever the view redraws.
        if (shape_) {
            seamDrawn_ = shape_->symmetrySeamVersion();
            const std::vector<Vec3f>& seam = shape_->symmetrySeam();
            if (seam.size() >= 2) {
                MPointArray points;
                points.setLength(static_cast<unsigned>(seam.size()));
                for (unsigned i = 0; i < seam.size(); ++i) points.set(MPoint(seam[i].x, seam[i].y, seam[i].z), i);
                draw.beginDrawable();
                draw.setColor(MColor(1.0f, 0.15f, 0.15f));
                draw.setLineWidth(2.0f);
                draw.setDepthPriority(5);
                draw.lineList(points, false);
                draw.endDrawable();
            }
        }
        addTubeWires(draw);
        // Viewing a node other than the output: say so, since nodes after it (width, color,
        // cards...) don't show until the output is viewed again.
        if (shape_ && !shape_->viewedNode().empty()) {
            int ox = 0, oy = 0, width = 0, height = 0;
            context.getViewportDimensions(ox, oy, width, height);
            const MString text = MString("GroomLab: viewing '") + shape_->viewedNode().c_str() +
                                 "', not the output (nodes after it don't show)";
            draw.beginDrawable();
            draw.setColor(MColor(1.0f, 0.75f, 0.2f));
            draw.setFontSize(MHWRender::MUIDrawManager::kSmallFontSize);
            draw.text2d(MPoint(static_cast<double>(ox + width / 2), static_cast<double>(oy + 36)), text,  // above the camera name
                        MHWRender::MUIDrawManager::kCenter);
            draw.endDrawable();
        }
    } catch (const std::exception&) {
        points_.clear();
        pointColors_.clear();
    }
}

std::array<float, 4> GroomSubSceneOverride::wireColor() const {
    MDagPath path;
    if (MDagPath::getAPathTo(object_, path) != MS::kSuccess) return {0.0f, 0.0f, 0.0f, 1.0f};
    const MColor c = MHWRender::MGeometryUtilities::wireframeColor(path);
    return {c.r, c.g, c.b, 1.0f};
}

void GroomSubSceneOverride::updateCardWires(MHWRender::MSubSceneContainer& container, bool rebuilt,
                                            const std::shared_ptr<const DisplayGeometry>& cards) {
    if (rebuilt) cardWires_ = cards && !cards->empty() ? std::make_shared<const DisplayGeometry>(cardWireGeometry(*cards)) : nullptr;
    const auto drawn = items_.find(kCardsItem.asChar());
    MHWRender::MRenderItem* cardsItem = container.find(kCardsItem);
    cardWiresShown_ = cardWires_ && cardsItem && drawn != items_.end() && drawn->second.shown;
    if (!cardWiresShown_) {
        if (!cardWires_) {
            const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            updateItem(container, kCardWiresItem, none, 0, nullptr, 1.0f);  // its buffers given back
        } else if (container.find(kCardWiresItem)) {
            hideItem(container, kCardWiresItem);
        }
        return;
    }
    cardWireColor_ = wireColor();
    const auto wire = items_.find(kCardWiresItem.asChar());
    if (rebuilt || !container.find(kCardWiresItem) || wire == items_.end() || !wire->second.shown) {
        updateItem(container, kCardWiresItem, cardWireColor_.data(), MHWRender::MRenderItem::sDormantWireDepthPriority, cardWires_, 1.0f);
    } else {
        restyleItem(container, kCardWiresItem, cardWireColor_.data(), 1.0f);
    }
    if (MHWRender::MRenderItem* item = container.find(kCardWiresItem)) {
        item->setDrawMode(MHWRender::MGeometry::kWireframe);
        item->enable(true);
    }
    cardsItem->setDrawMode(static_cast<MHWRender::MGeometry::DrawMode>(MHWRender::MGeometry::kShaded | MHWRender::MGeometry::kTextured));
}

void GroomSubSceneOverride::addTubeWires(MHWRender::MUIDrawManager& draw) {
    tubeWiresDrawn_ = shape_ && shape_->tubeWires();
    uiSelection_ = shape_ ? shape_->selectionHash() : 0;  // the selected tubes stand out
    if (!tubeWiresDrawn_) {
        if (!tubeWires_.empty() || !tubeWiresSelected_.empty()) tubeWires_ = {}, tubeWiresSelected_ = {};
        tubeWiresKey_ = ~std::uint64_t{0};
        return;
    }
    const HierarchyPtr hierarchy = shape_->evaluatedHierarchy();
    if (!hierarchy) return;
    const std::string hidden = MPlug(object_, GroomShape::aHiddenGuideGroups).asString().asChar();
    std::uint64_t key = hashCombine(hashCombine(shape_->hierarchyVersion(), shape_->selectionHash()), hashChars(0x7B3u, hidden));
    key = hashCombine(key, reinterpret_cast<std::uintptr_t>(hierarchy.get()));
    if (key != tubeWiresKey_) {
        tubeWiresKey_ = key;
        tubeWires_.clear();
        tubeWiresSelected_.clear();
        // Every guide the brush can shape (shown, not frozen: activeTubeGuides), the selected
        // ones (soft-selected left out) apart, to stand out.
        const std::map<GuideId, float> selection = shape_->guideSelectionWeights();
        const std::vector<const HierarchyGuide*> guides = activeTubeGuides(*hierarchy, hidden);
        std::size_t points = 0;
        for (const HierarchyGuide* g : guides) points += g->points.size();
        // Within a budget of lines: fewer rings along each tube on a groom of many guides.
        constexpr std::size_t kBudget = 400000;  // segment ends
        const std::size_t perPoint = 2 * 32 + 2 * 8;
        const std::size_t ringStep = std::max<std::size_t>(1, (points * perPoint + kBudget - 1) / kBudget);
        for (const HierarchyGuide* g : guides) {
            const auto it = selection.find(g->id);
            const bool selected = it != selection.end() && it->second >= 1.0f;
            appendTubeWire(selected ? tubeWiresSelected_ : tubeWires_, *g, hierarchy->scale(), 8, ringStep);
        }
    }
    auto lines = [&](const std::vector<Vec3f>& wire, const MColor& color, float width) {
        if (wire.size() < 2) return;
        MPointArray points;
        points.setLength(static_cast<unsigned>(wire.size()));
        for (unsigned i = 0; i < wire.size(); ++i) points.set(MPoint(wire[i].x, wire[i].y, wire[i].z), i);
        draw.beginDrawable();
        draw.setColor(color);
        draw.setLineWidth(width);
        draw.setDepthPriority(5);
        draw.lineList(points, false);
        draw.endDrawable();
    };
    lines(tubeWires_, MColor(0.45f, 0.85f, 1.0f, 0.6f), 1.0f);
    lines(tubeWiresSelected_, MColor(1.0f, 0.7f, 0.15f, 1.0f), 2.0f);  // the selected: warm, opaque, thicker
}

void GroomSubSceneOverride::addDots(MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& context) {
    if (points_.empty() || pointSize_ <= 0.0f) return;
    int ox = 0, oy = 0, width = 0, height = 0;
    context.getViewportDimensions(ox, oy, width, height);
    if (height <= 0) return;
    const MMatrix camera = context.getMatrix(MHWRender::MFrameContext::kViewInverseMtx);
    const MMatrix projection = context.getMatrix(MHWRender::MFrameContext::kProjectionMtx);
    const MVector right(camera(0, 0), camera(0, 1), camera(0, 2)), up(camera(1, 0), camera(1, 1), camera(1, 2));
    const MVector back(camera(2, 0), camera(2, 1), camera(2, 2));  // cameras look down -Z
    const MPoint eye(camera(3, 0), camera(3, 1), camera(3, 2));
    const bool ortho = std::fabs(projection(3, 3) - 1.0) < 1e-9;
    // World units per pixel at view depth d: 2 d / (P11 h) in perspective, 2 / (P11 h) orthographic.
    const double perPixel = 2.0 / (projection(1, 1) * static_cast<double>(height));
    constexpr int kSegments = 8;
    MPointArray positions;
    MColorArray colors;
    MUintArray indices;
    positions.setSizeIncrement(static_cast<unsigned>(points_.size() * (kSegments + 1)));
    // With the guides over the hair, the dots go with them: toward the camera by the hair's
    // depth, short of anything in the scene (the lines' pull, on the CPU here).
    const bool onTop = displayFlag(GroomShape::aDisplayOverlaysOnTop) && overlayPull_ > 0.0f && !ortho;
    const Vec3f eyeAt(static_cast<float>(eye.x), static_cast<float>(eye.y), static_cast<float>(eye.z));
    const std::vector<float> limits = onTop ? overlayPullLimits(points_, eyeAt, overlayPull_, occluderMeshes_) : std::vector<float>();
    for (std::size_t i = 0; i < points_.size(); ++i) {
        MPoint center(points_[i].x, points_[i].y, points_[i].z);  // world space
        if (onTop) {
            const MVector toEye = eye - center;
            const double away = toEye.length();
            if (away > 1e-6) center += toEye / away * std::min<double>(overlayPull_ * limits[i], 0.5 * away);
        }
        const double depth = ortho ? 1.0 : std::max(1e-6, -(center - eye) * back);
        const double radius = 0.5 * pointSize_ * perPixel * depth;
        const unsigned base = positions.length();
        const MColor color(pointColors_[i][0], pointColors_[i][1], pointColors_[i][2]);
        positions.append(center);
        colors.append(color);
        for (int k = 0; k < kSegments; ++k) {
            const double a = 6.283185307179586 * k / kSegments;
            positions.append(center + (right * std::cos(a) + up * std::sin(a)) * radius);
            colors.append(color);
            indices.append(base);
            indices.append(base + 1 + static_cast<unsigned>(k));
            indices.append(base + 1 + static_cast<unsigned>((k + 1) % kSegments));
        }
    }
    draw.beginDrawable();
    draw.setDepthPriority(MHWRender::MRenderItem::sActiveWireDepthPriority);
    draw.mesh(MHWRender::MUIDrawManager::kTriangles, positions, nullptr, &colors, &indices);
    draw.endDrawable();
}

void GroomSubSceneOverride::forgetDrawn() {
    for (auto& entry : items_) releaseGeometry(entry.second);
    // The level of detail's items went with the container: their indices and shaders too.
    if (const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer()) {
        if (const MHWRender::MShaderManager* shaders = renderer->getShaderManager()) {
            for (MHWRender::MShaderInstance*& shader : lodShaders_) {
                if (shader) shaders->releaseShader(shader);
                shader = nullptr;
            }
        }
    }
    for (auto& indices : lodIndices_) indices.reset();
    lodSlotKeys_ = {};
    lodBound_ = {};
    lodOn_ = false;
    lodKey_ = lodImpostorKey_ = lodImpostorColors_ = 0;
    lodDrawn_ = StrandLodView{};
    lodLines_ = {};
    gpuBound_ = false;  // the items went with the container
    gpuBoundsSet_ = false;
    gpuStale_ = true;
    builder_.invalidateAll();
    boundStreams_.clear();
    overlayBatches_.clear();
    drawnVersion_ = ~std::uint64_t{0};
    drawnDisplay_ = ~std::uint64_t{0};
    drawnSelection_ = ~std::uint64_t{0};
    drawnTheme_ = 0;
    colorDirty_ = true;
    points_.clear();
    pointColors_.clear();
    uiDirty_ = true;
}

void GroomSubSceneOverride::update(MHWRender::MSubSceneContainer& container, const MHWRender::MFrameContext& context) {
    // Nothing may throw into Maya's draw loop: out of memory on a huge preview, say,
    // clears the drawing and says so instead of ending the session.
    try {
        currentDraw() = DrawTimes{};
        Lap lap;
        updateGuarded(container, context);
        currentDraw().updateMs = lap();
        if (!currentDraw().items.empty()) {
            currentDraw().serial = lastDraw().serial + 1;
            lastDraw() = currentDraw();
        }
    } catch (const std::exception& e) {
        // The items come back (rebuilt) on the next update: the versions are forgotten
        // too, or requiresUpdate would find nothing changed and the viewport stay empty.
        container.clear();
        forgetDrawn();
        MGlobal::displayWarning(MString("GroomLab: the viewport could not draw the groom (") + e.what() +
                                "); lower previewDensity or previewPointBudget.");
    }
}

DisplaySettings GroomSubSceneOverride::readSettings() {
    DisplaySettings s;
    // Maya's hair tube shading (no hair BSDF) draws the groom's colors as the hair looks
    // (hairAppearanceColor); GroomLab's hair shading takes their base color.
    s.appearanceColors = !hairShading_;
    s.hairColor = drawnGroomColors_ && !hairShading_ ? hairAppearanceColor(drawnColor_) : drawnColor_;
    s.strandLookColors = drawnGroomColors_;  // a material's color (or the display color) is every strand's
    s.colorMode = MPlug(object_, GroomShape::aStrandColorMode).asShort();
    s.colorSource = MPlug(object_, GroomShape::aStrandColorSource).asString().asChar();
    s.colorMin = floatPlug(GroomShape::aStrandColorMin);
    s.colorMax = floatPlug(GroomShape::aStrandColorMax);
    s.ribbons = displayFlag(GroomShape::aDisplayRibbons);
    s.ribbonWidthScale = floatPlug(GroomShape::aRibbonWidthScale);
    s.widthCoverage = widthCoverage_;
    s.coverageDensity = displayFlag(GroomShape::aDisplayCoverageDensity);
    s.lighting = displayFlag(GroomShape::aDisplayLighting);
    s.sculptDisplay = MPlug(object_, GroomShape::aSculptDisplay).asShort();
    // Cards view: the cards show instead of the strands (drawn here only without a card mesh,
    // which shows them itself).
    s.cardsView = shape_->cardsView();
    s.drawCards = s.cardsView && !shape_->cardMeshConnected();
    // The StrandMesh nodes' meshes instead of the strands (drawn here unless a mesh on
    // outStrandMesh shows them).
    s.strandMeshView = displayFlag(GroomShape::aDisplayStrandMesh);
    s.drawStrandMesh = s.strandMeshView && !shape_->strandMeshConnected();
    s.instances = displayFlag(GroomShape::aDisplayInstances);
    s.hair = displayFlag(GroomShape::aDisplayHair);  // (the eye: the guides draw either way)
    s.guides = displayFlag(GroomShape::aDisplayGuides);
    s.tubes = displayFlag(GroomShape::aDisplayTubes);
    s.guideWidth = floatPlug(GroomShape::aGuideWidth);
    s.guideColorMode = MPlug(object_, GroomShape::aGuideColorMode).asShort();
    s.guideColorLayer = MPlug(object_, GroomShape::aGuideColorLayer).asString().asChar();
    s.isolateGroup = MPlug(object_, GroomShape::aIsolateGroup).asString().asChar();
    s.guidePoints = MPlug(object_, GroomShape::aGuidePoints).asShort();
    s.guideSmoothing = MPlug(object_, GroomShape::aGuideSmoothing).asInt();
    s.guideDisplaySegments = MPlug(object_, GroomShape::aGuideDisplaySegments).asInt();
    s.hiddenGuideGroups = MPlug(object_, GroomShape::aHiddenGuideGroups).asString().asChar();
    s.guideGroupColors = MPlug(object_, GroomShape::aGuideGroupColors).asString().asChar();  // "name:#rrggbb ..."
    s.guideColorSeed = guideColorSeedOf(object_);  // the groom's own hue: two grooms' groups differ
    s.flow = displayFlag(GroomShape::aDisplayFlow);
    s.theme = displayTheme();
    return s;
}

void GroomSubSceneOverride::updateGuarded(MHWRender::MSubSceneContainer& container, const MHWRender::MFrameContext& context) {
    if (!shape_ || !MHWRender::MRenderer::theRenderer()) return;
    {
        const MMatrix view = context.getMatrix(MHWRender::MFrameContext::kViewInverseMtx);
        overlayCamera_ = {static_cast<float>(view(3, 0)), static_cast<float>(view(3, 1)), static_cast<float>(view(3, 2))};
        overlayCameraKnown_ = true;
        overlayFresh_.clear();
    }
    // Hidden (itself, a parent, a display layer): no items; shown again, all rebuilt.
    drawnVisible_ = visible();
    if (!drawnVisible_) {
        container.clear();
        forgetDrawn();
        return;
    }
    // A commit evaluating off the UI thread (the evaluation pulled here may start one): the items
    // stay as they are, drawing the GPU preview's results (the commit's look), until it lands. (An
    // update asked for meanwhile, a camera's new level of detail say, waits for that too.)
    Lap pulled;
    shape_->handedStrands();
    currentDraw().pullMs = pulled();
    if (shape_->commitPending()) return;
    shape_->expectCommit(GroomShape::CommitKind::None);  // (one that did not come with that pull: stale)

    const DisplayTheme& theme = displayTheme();
    drawnTheme_ = theme.version;
    drawnVersion_ = shape_->evaluationVersion();
    drawnDisplay_ = shape_->displayVersion();
    drawnSelection_ = shape_->selectionHash();
    // The groom scaled with its scalp to another size: drawn afresh (drawnScale_).
    if (const double k = shape_->groomScale(); k != drawnScale_) {
        if (drawnScale_ != 0.0) drawAfreshForScale(container);
        drawnScale_ = k;
    }
    // Width as coverage turned on or off: the strand lines are written again for their shaders.
    const bool coverage = displayFlag(GroomShape::aDisplayWidthCoverage);
    if (coverage != widthCoverage_) {
        widthCoverage_ = coverage;
        builder_.invalidate(DisplayPart::Strands);
    }
    // Ribbons at their true width where the GPU makes them (else the CPU's along the scalp): the
    // strands built again when that changes.
    if (const bool facing = displayFlag(GroomShape::aDisplayRibbons) && gpuLines_ && gpuLines_->engine(); facing != facing_) {
        facing_ = facing;
        builder_.invalidate(DisplayPart::Strands);
    }
    if (facing_) readFacingView(context);
    // GroomLab's hair shading or Maya's: the lit strands are built again (their shaders) when it changes.
    {
        const MHWRender::MShaderManager* shaders = MHWRender::MRenderer::theRenderer()->getShaderManager();
        const bool hair = MPlug(object_, GroomShape::aDisplayHairShading).asShort() == 1 && shaders && hairShaderAvailable(*shaders);
        if (hair != hairShading_) {
            hairShading_ = hair;
            builder_.invalidate(DisplayPart::Strands);
            builder_.invalidate(DisplayPart::Cards);  // (the cards shade with it too: their shader swapped)
        }
        // The lights the groom ignores (light linking), looked at again now and then.
        const auto now = std::chrono::steady_clock::now();
        if (hair && now - lightsChecked_ > std::chrono::seconds(1)) {
            lightsChecked_ = now;
            if (!hairState_) hairState_ = std::make_shared<HairGroomState>();
            MDagPath path;
            if (MDagPath::getAPathTo(object_, path)) hairState_->ignoredLights = ignoredLights(path);
        }
    }
    MObject engine, material;
    drawnColor_ = litColor(&engine, &material, &drawnGroomColors_);
    watchMaterial(engine, material);
    colorDirty_ = false;
    DisplaySettings settings = readSettings();
    // Following the scalp: where the GPU can carry the strands (followDrawable), the frame hands them
    // on at rest (the shape asks for that from now on) and the builder holds them at rest, built again
    // only when those change; else the strands where they are (carried by the CPU on demand).
    const bool stroke = shape_->hasPreview();
    const FollowCarryPtr follow = shape_->evaluatedFollow();
    const StrandsPtr handed = shape_->handedStrands();
    gpu::LineFrames* lineEngine = !followBroken_ && gpuLines_ && (follow || MPlug(object_, GroomShape::aFollowScalp).asBool())
                                      ? gpuLines_->engine() : nullptr;
    const bool carries = lineEngine && followDrawable(settings, handed.get(), stroke);
    shape_->setViewportCarries(carries);
    follow_ = carries && follow && handed && handed->pointCount() > 0 ? follow : nullptr;
    if (follow_) {
        if (follow_->rest() != followRest_) {
            static std::uint64_t restCounter = 0;
            followRest_ = follow_->rest();
            followRestVersion_ = (std::uint64_t{0xF0110} << 40) | ++restCounter;  // (never a shape's strandsVersion)
        }
        // The roots on the rest pose: found again for other roots (strands evaluated at rest on every
        // frame, a Simulate node's, share theirs) or another rest pose.
        const bool sameRoots = followRootsOf_ && followRootsOf_->roots().data() == followRest_->roots().data() &&
                               followRootsOf_->strandCount() == followRest_->strandCount();
        if (followRootsFrom_ != follow_->from() || !sameRoots) {
            followRoots_.resize(followRest_->strandCount());
            followRootsKey_ = writeFollowRoots(*followRest_, *follow_->from(), followRoots_.data());
            followRootsFrom_ = follow_->from();
        }
        followRootsOf_ = followRest_;  // (held: its roots' memory is not another's while compared)
        // The strands' buffers hold carried points: no later strands are written into them in part.
        builder_.forgetHeld(DisplayItemId::Strands);
    } else if (followRest_) {
        followRest_.reset();
        followRootsOf_.reset();
        followRootsFrom_.reset();
        followRoots_ = {};
        followRootsKey_ = 0;
    }
    const StrandsPtr strands = follow_ ? handed : shape_->evaluatedStrands();
    // What the builder draws with that only the scene has: each part rebuilds for its own
    // changes (a brush stamp re-uploads the guides, then the strands, never the parts; a
    // static groom at another frame nothing at all).
    DisplayInputs in;
    in.strands = strands;
    in.viewedRoots = shape_->showsRoots();  // a Scatter shown alone: its roots as dots
    in.clumpCenters = shape_->clumpCenters();  // a Clump shown: its centers as dots
    in.scalp = shape_->evaluatedScalp();
    in.hierarchy = shape_->evaluatedHierarchy();
    in.parts = shape_->evaluatedParts();
    in.flow = shape_->evaluatedFlow();
    in.cards = shape_->evaluatedCards();
    in.strandMeshes = shape_->evaluatedStrandMeshes();
    // (Strands at rest the GPU carries: the same strands whatever the scalp, written whole when they change.)
    in.strandsVersion = follow_ ? followRestVersion_ : shape_->strandsVersion();
    in.strandsSerial = follow_ ? 0 : shape_->strandsSerial();
    in.changesBase = follow_ ? 0 : shape_->strandsChangesBase();
    in.strandsChanges = follow_ ? nullptr : shape_->strandsChanges();
    in.follow = follow_;  // the instances placed on their strands carried (the strands at rest, carried by the GPU)
    in.hierarchyVersion = shape_->hierarchyVersion();
    in.cardsVersion = shape_->cardsVersion();
    in.strandMeshVersion = shape_->strandMeshVersion();
    in.stroke = stroke;  // sculpting: the strands step back so the guides read
    in.selection = [this] { return shape_->guideSelectionWeights(); };
    in.selectionHash = shape_->selectionHash();
    // Instances: each its prototype mesh, from the scene by name (refetched when it changed).
    const int sculpt = !settings.hair ? 2 : in.stroke ? settings.sculptDisplay : 0;  // (hidden: as the builder's hairDisplay)
    const InstanceSet* instances = strands && sculpt != 2 && settings.instances ? strands->instances().get() : nullptr;
    if (instances) {
        refreshPrototypes(instances->prototypes);
    } else {
        prototypes_.clear();
    }
    in.prototypes = &prototypes_;
    // The cards' baked maps (once baked, displayCardTextures on): loaded or reloaded when rewritten.
    const bool wantTextures = settings.drawCards && displayFlag(GroomShape::aDisplayCardTextures);
    const std::string cardMaps = wantTextures ? MPlug(object_, GroomShape::aCardMaps).asString().asChar() : std::string();
    const bool textured = wantTextures && loadCardTextures(cardMaps);
    if (!wantTextures) releaseCardTextures();  // not shown: give the textures back
    settings.cardTextures = textured;
    cardsTextured_ = textured;
    // The map shown on the scalp (Groom Tools > Paint): a texture map's image as a texture of its own.
    const GroomShape::ScalpMapView view = shape_->scalpMapView();
    scalpMapVersion_ = view.version;
    const bool mapOnScalp = !view.kind.empty() && in.scalp;
    MHWRender::MTexture* texture = mapOnScalp && view.kind == "texture" ? scalpMapTexture(view) : nullptr;
    if (!mapOnScalp) releaseScalpMapTexture();
    in.scalpMap = view;
    in.scalpMapTexture = texture != nullptr;
    in.scalpMapTextureGeneration = scalpMapTextureGeneration_;
    // A new color range or source re-uploads the colored lines' colors alone while they are as built.
    in.recolorable = strands && recolorable(container, strands->pointCount());
    // A GPU preview (a drag, a stroke's dabs) ended: the strands are drawn from the CPU's again.
    // Where it wrote the item's own buffers (OpenGL in place, a copy), that item is written whole:
    // a stroke's dimmed strands when they show next, the strands now. (The preview's own buffers
    // leave the item's as the CPU wrote them: it binds them again, applyGpuPreview.)
    if (gpuWrote_ && !gpuRunning()) {
        if (gpuWroteItem_) {
            if (gpuItem_ == kDimStrandsItem) {
                builder_.forgetHeld(DisplayItemId::DimStrands);
            } else {
                builder_.invalidate(DisplayPart::Strands);
            }
        }
        gpuWrote_ = gpuWroteItem_ = false;
    }
    // The brush tool put away (GroomShape::strokeKept): the dimmed strands kept hidden for the
    // next stroke give their buffers back (at 500k strands and the default sculptDensity, ~55 MB
    // of video memory, and its system-memory copy under DirectX 11); the next stroke writes them whole.
    if (!shape_->strokeKept() && builder_.forgetKept(DisplayItemId::DimStrands)) {
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        updateItem(container, kItemNames[static_cast<std::size_t>(DisplayItemId::DimStrands)], none, 0, nullptr, 1.0f);
    }
    // Items Maya dropped are built again.
    for (const DisplayItem& item : builder_.frame().items) {
        const MString& name = kItemNames[static_cast<std::size_t>(item.id)];
        auto it = items_.find(name.asChar());
        if (it != items_.end() && it->second.shown && !container.find(name)) builder_.invalidate(item.part);
    }
    currentDraw().settingsMs = pulled();

    Lap lap;
    const DisplayFrame& frame = builder_.update(in, settings);
    currentDraw().buildMs = lap();
    // How deep the hair is, for the overlay effect's pull, and what still hides the overlays
    // within that pull of the groom (throttled).
    overlayPull_ = frame.overlayPull;
    refreshOccluders();
    pointSize_ = frame.pointSize;
    if (frame.rebuilt[static_cast<std::size_t>(DisplayPart::Guides)]) {
        points_ = frame.dots.points;
        pointColors_ = frame.dots.colors;
        lastDots().dots = points_.size();
        lastDots().clumpCenters = static_cast<std::size_t>(
            std::count(pointColors_.begin(), pointColors_.end(), theme.clumpCenters));
    }
    bool cardsRebuilt = false;
    std::shared_ptr<const DisplayGeometry> cards;
    for (const DisplayItem& item : frame.items) {
        const MString& name = kItemNames[static_cast<std::size_t>(item.id)];
        if (item.id == DisplayItemId::Cards && item.rebuilt && !item.keep) cardsRebuilt = true, cards = item.geometry;
        // The strand lines as built, for the level of detail (the builder keeps none).
        if (item.rebuilt && (item.id == DisplayItemId::Strands || item.id == DisplayItemId::ColoredStrands)) {
            lodLines_[item.id == DisplayItemId::Strands ? 0 : 1] =
                item.geometry && !item.geometry->ribbons ? item.geometry->lines : nullptr;
        }
        if (item.rebuilt && item.keep) {
            hideItem(container, name);  // stepping back for a stroke (or back after it): its buffers kept
        } else if (item.rebuilt) {
            // Only the strands that changed since what its buffers hold, where it holds them.
            if (!item.updated || !updateItemPartly(container, name, item.color.data(), item)) {
                updateItem(container, name, item.color.data(), depthPriority(item.layer), item.geometry, item.lineWidth,
                           item.id == DisplayItemId::ScalpMap ? texture : nullptr);
            }
        } else if (item.recolor) {
            recolorStrands(container, *item.recolored, item.color.data());
        } else if (item.id == DisplayItemId::ScalpMap) {
            // The same geometry with the map's image as it is now (a stroke's preview, a rewritten file).
            ItemBuffers& map = items_[name.asChar()];
            if (texture && map.shader && map.shaderKind == 8) bindItemTexture(map, texture);
        } else {
            restyleItem(container, name, item.color.data(), item.lineWidth);
        }
    }
    updateCardWires(container, cardsRebuilt, cards);
    currentDraw().uploadMs = lap();
    // The maps (reloaded above when rewritten) go to the shader every time: cheap, and a
    // rebake with the same paths shows without rebuilding the cards. Without them: solid.
    {
        ItemBuffers& cards = items_[kCardsItem.asChar()];
        if (textured) bindCardTextures(cards);
        if (cards.shader && cards.shaderKind == 6) {
            cards.shader->setParameter("solid", textured ? 0.0f : 1.0f);
            if (!cards.hair) cards.shader->setParameter("coverageSolid", textured ? 0.0f : 1.0f);  // (the graph's coverage fragment)
            // Out of the post effects while textured (style()'s notes): the maps come and go without
            // the item restyled.
            MHWRender::MRenderItem* item = container.find(kCardsItem);
            if (item && item->excludedFromPostEffects() != textured) item->setExcludedFromPostEffects(textured);
        }
    }
    // Overlay items the camera moved past (not rebuilt just now): their pull limits again.
    if (!occluders_.empty() && displayFlag(GroomShape::aDisplayOverlaysOnTop)) {
        for (auto& [name, kept] : overlayBatches_) {
            if (overlayFresh_.count(name)) continue;
            auto it = items_.find(name);
            if (it == items_.end() || !it->second.shown || it->second.shaderKind != 7) continue;
            uploadOverlayColors(it->second, *kept, overlayPullLimits(*kept, overlayCamera_, overlayPull_, occluderMeshes_));
        }
    }
    // Following the scalp on the GPU: the strands carried to this frame's scalp where the builder
    // wrote nothing; where the GPU could not, the CPU's strands from the next update.
    if (follow_ && !followBroken_) carryFollowed(container);
    if (follow_ && followBroken_) {
        shape_->setViewportCarries(false);
        builder_.invalidate(DisplayPart::Strands);
        drawnVersion_ = ~std::uint64_t{0};
        MHWRender::MRenderer::setGeometryDrawDirty(object_);
    }
    uiDirty_ = true;  // the dots (and the viewed-node note) follow what was rebuilt
    currentDraw().afterMs = lap();
    Lap phases;
    if (gpuPreview_) applyGpuPreview(container, strands);
    currentDraw().previewMs = phases();
    // Ribbons facing the camera: made again for this view where nothing above made them.
    if (facing_) refaceRibbons();
    currentDraw().facing = facing_;
    // The level of detail last: it binds the streams the strands' item draws now.
    updateLod(container, context);
    currentDraw().lodMs = phases();
    updateHairGrid(context);
    currentDraw().gridMs = phases();
}

void GroomSubSceneOverride::drawAfreshForScale(MHWRender::MSubSceneContainer& container) {
    // What width as coverage turned off and on does (the user's way back from hair that drew darker
    // after the scalp was scaled down, frozen and scaled back up), for the strands' items alone.
    hideLod(container);
    for (const MString* name : {&kStrandsItem, &kDimStrandsItem, &kColoredStrandsItem, &kRibbonsItem}) {
        const auto it = items_.find(name->asChar());
        if (it == items_.end()) continue;
        ItemBuffers& b = it->second;
        b.shaderKind = -1;  // its shader acquired again
        b.layout = b.uvsKey = b.uvsIdsKey = b.colorsKey = 0;  // every stream written
        b.ribbonLayout = b.ribbonUvsKey = b.ribbonColorsKey = b.ribbonIdsKey = 0;
    }
    builder_.invalidate(DisplayPart::Strands);
    hairGridKey_ = 0;
    // The self-shadowing grid in a texture of its own (its cells another size in the world: the hair
    // shaders let go of the old texture and its old density, setHairGrid).
    if (hairState_) {
        hairState_->valid = false;
        hairState_->remake = true;
    }
}

void gpuFramesSwitched() {
    for (auto& [shape, o] : drawnOverrides()) {
        o->builder_.invalidate(DisplayPart::Strands);
        o->drawnVersion_ = ~std::uint64_t{0};  // (requiresUpdate: something to draw again)
        MHWRender::MRenderer::setGeometryDrawDirty(o->object_);
    }
}
}  // namespace groomlab::maya
