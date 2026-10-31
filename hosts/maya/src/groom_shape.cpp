// SPDX-License-Identifier: Apache-2.0
#include "groom_shape.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <maya/MAnimControl.h>
#include <maya/MArrayDataHandle.h>
#include <maya/MColorArray.h>
#include <maya/MDoubleArray.h>
#include <maya/MEventMessage.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnCompoundAttribute.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnEnumAttribute.h>
#include <maya/MFnGeometryData.h>
#include <maya/MFnMesh.h>
#include <maya/MFnMeshData.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnNurbsCurve.h>
#include <maya/MFnPluginData.h>
#include <maya/MFnStringArrayData.h>
#include <maya/MFnStringData.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnUnitAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MMatrix.h>
#include <maya/MFileIO.h>
#include <maya/MNodeMessage.h>
#include <maya/MPlugArray.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/M3dView.h>
#include <maya/MTime.h>
#include <maya/MVectorArray.h>
#include <maya/MViewport2Renderer.h>

#include "groomlab/feathers.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_selection.h"
#include "groomlab/parallel.h"
#include "groomlab/png.h"
#include "groomlab/random.h"
#include "groomlab/render_overrides.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/strand_lod.h"
#include "groomlab/texture.h"
#include "asset_files.h"
#include "gpu_preview.h"
#include "maya_inputs.h"
#include "node_ids.h"

namespace groomlab::maya {

namespace {

std::uint64_t hashMatrix(std::uint64_t h, const MMatrix& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) h = hashFloat(h, static_cast<float>(m(r, c)));
    }
    return h;
}

// A cheap fingerprint of mesh data: its points (read in place, no copy), counts
// and matrix. UVs and vertex colors are left out: editing those dirties the
// plug, which is tracked separately (Lane::meshDirty).
std::uint64_t meshFingerprint(std::uint64_t h, const MObject& data) {
    if (data.isNull()) return hashCombine(h, 0);
    MStatus status;
    MFnMesh fn(data, &status);
    if (!status) return hashCombine(h, 1);
    const int n = fn.numVertices();
    h = hashCombine(h, static_cast<std::uint64_t>(n));
    h = hashCombine(h, static_cast<std::uint64_t>(fn.numPolygons()));
    const float* raw = fn.getRawPoints(&status);
    if (status && raw) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n) * 3; ++i) h = hashFloat(h, raw[i]);
    }
    MMatrix m;
    if (MFnGeometryData(data).getMatrix(m) == MS::kSuccess) h = hashMatrix(h, m);
    return h;
}

// A fingerprint of everything the guides' sampling reads of curve data: its CVs, knots,
// degree, form and matrix.
std::uint64_t curveFingerprint(std::uint64_t h, const MObject& data) {
    MStatus status;
    MFnNurbsCurve fn(data, &status);
    if (!status) return hashCombine(h, 1);
    MPointArray cvs;
    fn.getCVs(cvs, MSpace::kObject);
    h = hashCombine(h, cvs.length());
    h = hashCombine(h, static_cast<std::uint64_t>(fn.degree()));
    h = hashCombine(h, static_cast<std::uint64_t>(fn.form()));
    auto bits = [](double v) {
        std::uint64_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        return b;
    };
    for (unsigned i = 0; i < cvs.length(); ++i) {
        h = hashCombine(hashCombine(hashCombine(hashCombine(h, bits(cvs[i].x)), bits(cvs[i].y)), bits(cvs[i].z)),
                        bits(cvs[i].w));
    }
    MDoubleArray knots;
    fn.getKnots(knots);
    h = hashCombine(h, knots.length());
    for (unsigned i = 0; i < knots.length(); ++i) h = hashCombine(h, bits(knots[i]));
    MMatrix m;
    if (MFnGeometryData(data).getMatrix(m) == MS::kSuccess) h = hashMatrix(h, m);
    return h;
}

}  // namespace

const char* GroomShape::kTypeName = "groomLabShape";
const MTypeId GroomShape::kTypeId(kGroomLabShapeTypeId);
// "drawdb/subscene/..." routes VP2 to an MPxSubSceneOverride; a
// "drawdb/geometry/..." classification here crashes VP2 when the node is drawn.
const MString GroomShape::kDrawDbClassification("drawdb/subscene/groomLabShape");

MObject GroomShape::aInMesh;
MObject GroomShape::aRestMesh;
MObject GroomShape::aFollowScalp;
MObject GroomShape::aColliderMeshes;
MObject GroomShape::aColliderMesh;
MObject GroomShape::aColliderName;
MObject GroomShape::aColliderRestMesh;
MObject GroomShape::aExtraScalps;
MObject GroomShape::aExtraScalpMesh;
MObject GroomShape::aExtraScalpName;
MObject GroomShape::aExtraScalpRestMesh;
MObject GroomShape::aRenderDensity;
MObject GroomShape::aRenderWidthScale;
MObject GroomShape::aRenderLengthScale;
MObject GroomShape::aRenderView;
MObject GroomShape::aSimGuides;
MObject GroomShape::aSimCurve;
MObject GroomShape::aSimGuideId;
MObject GroomShape::aGuideCache;
MObject GroomShape::aGuideCacheObject;
MObject GroomShape::aGuideCurves;
MObject GroomShape::aTime;
MObject GroomShape::aSimCaches;
MObject GroomShape::aTransferMarkers;
MObject GroomShape::aScalpTopology;
MObject GroomShape::aScaleSource;
MObject GroomShape::aScaleReference;
MObject GroomShape::aGroomScale;
MObject GroomShape::aScaleArea;
MObject GroomShape::aScaleTransformReference;
MObject GroomShape::aScaleTransform;
MObject GroomShape::aGraph;
MObject GroomShape::aGuideData;
MObject GroomShape::aPartData;
MObject GroomShape::aGraphUi;
MObject GroomShape::aViewNode;
MObject GroomShape::aLocked;
MObject GroomShape::aDisplayInstances;
MObject GroomShape::aCardDensity;
MObject GroomShape::aOutCards;
MObject GroomShape::aOutRachis;
MObject GroomShape::aOutMesh;
MObject GroomShape::aDisplayCards;
MObject GroomShape::aDisplayMesh;
MObject GroomShape::aCardMaps;
MObject GroomShape::aDisplayCardTextures;
MObject GroomShape::aDisplayOverlaysOnTop;
MObject GroomShape::aRenderMode;
MObject GroomShape::aStrandColorMode;
MObject GroomShape::aStrandColorSource;
MObject GroomShape::aStrandColorMin;
MObject GroomShape::aStrandColorMax;
MObject GroomShape::aDisplayRibbons;
MObject GroomShape::aDisplayWidthCoverage;
MObject GroomShape::aDisplayCoverageDensity;
MObject GroomShape::aDisplayClumpLod;
MObject GroomShape::aClumpLodError;
MObject GroomShape::aSculptDisplay;
MObject GroomShape::aSculptDensity;
MObject GroomShape::aIsolateGroup;
MObject GroomShape::aGuideColorMode;
MObject GroomShape::aGuideColorLayer;
MObject GroomShape::aRibbonWidthScale;
MObject GroomShape::aDisplayLighting;
MObject GroomShape::aGuideWidth;
MObject GroomShape::aCorrectiveData;
MObject GroomShape::aSculptData;
MObject GroomShape::aSculptNode;
MObject GroomShape::aFlowData;
MObject GroomShape::aScalpMaps;
MObject GroomShape::aScalpMapDisplay;
MObject GroomShape::aScalpMapChannel;
MObject GroomShape::aDisplayFlow;
MObject GroomShape::aShotSculpt;
MObject GroomShape::aCorrectiveWeight;
MObject GroomShape::aShotLayer;
MObject GroomShape::aLodDistance;
MObject GroomShape::aLodNear;
MObject GroomShape::aLodFar;
MObject GroomShape::aLodMinDensity;
MObject GroomShape::aLodWidthCompensation;
MObject GroomShape::aCameraCull;
MObject GroomShape::aCullMargin;
MObject GroomShape::aBackfaceCull;
MObject GroomShape::aBackfaceAngle;
MObject GroomShape::aDisplayShadows;
MObject GroomShape::aDisplayHairShading;
MObject GroomShape::aDisplaySelfShadowing;
MObject GroomShape::aDisplayStrandVariation;
MObject GroomShape::aDisplayColor;
MObject GroomShape::aUseMaterialColor;
MObject GroomShape::aGuidePoints;
MObject GroomShape::aGuideSmoothing;
MObject GroomShape::aHiddenGuideGroups;
MObject GroomShape::aGuideGroupColors;
MObject GroomShape::aGuideSelection;
MObject GroomShape::aSoftSelectRadius;
MObject GroomShape::aGroomFile;
MObject GroomShape::aPreviewDensity;
MObject GroomShape::aPreviewPointBudget;
MObject GroomShape::aDisplayGuides;
MObject GroomShape::aDisplayTubes;
MObject GroomShape::aDisplayHair;
MObject GroomShape::aBrushTick;
MObject GroomShape::aOutStrandCount;
MObject GroomShape::aLastError;
MObject GroomShape::aOutFrame;

const MTypeId GroomFrameData::kTypeId(kGroomLabFrameDataTypeId);
const MString GroomFrameData::kTypeName("groomLabFrameData");

namespace {

// GroomFrame's versions: one counter for every groom and lane, so two frames never share a version
// for different content (a cached frame drawn after a live one is told apart).
std::uint64_t nextVersion() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

// A numeric data handle's value as a float (a control: a double, a long, a bool or a float).
float handleAsFloat(MDataHandle& handle) {
    switch (handle.numericType()) {
        case MFnNumericData::kDouble: return static_cast<float>(handle.asDouble());
        case MFnNumericData::kFloat: return handle.asFloat();
        case MFnNumericData::kBoolean: return handle.asBool() ? 1.0f : 0.0f;
        case MFnNumericData::kShort: return static_cast<float>(handle.asShort());
        case MFnNumericData::kInt: return static_cast<float>(handle.asInt());
        case MFnNumericData::kInt64: return static_cast<float>(handle.asInt64());
        case MFnNumericData::kByte:
        case MFnNumericData::kChar: return static_cast<float>(handle.asChar());
        default: return 0.0f;
    }
}

}  // namespace

void* GroomShape::creator() { return new GroomShape(); }

GroomShape::GroomShape() : background_(std::make_unique<BackgroundLane>()) {
    // Frames in any order: a Simulate node shows its bake (or its guides as groomed), never steps.
    background_->session.setSimulationStepping(false);
}

namespace {
GroomShape* gBrushTarget = nullptr;  // the groom the brush tool is on
MCallbackId gWarmCallback = 0;       // a one-off idle callback, while a warm-up is due
std::set<GroomShape*> gPendingCommits;  // grooms whose commit evaluates off the UI thread
MCallbackId gLandCallback = 0;           // the idle callback landing them, while there is one
std::set<GroomShape*> gPendingWarms;     // grooms whose stroke warm-up evaluates off the UI thread
MCallbackId gAdoptCallback = 0;          // the idle callback taking them in, while there is one

// Threads a detached evaluation's parallel loops take (a commit's, a warm-up's): three quarters of
// the cores, so Maya's drawing (and its own TBB loops) keep the rest while the worker evaluates (on
// every core, a 500k-strand redraw took ~50 ms instead of ~5). GROOMLAB_ASYNC_THREADS sets another
// number (0: every core).
// GROOMLAB_STROKE_WARMUP=0 in the environment: no stroke document warm-up (comparisons, the
// benchmark's first press).
bool strokeWarmupOff() {
    static const bool off = [] {
        const char* value = std::getenv("GROOMLAB_STROKE_WARMUP");
        return value && std::string(value) == "0";
    }();
    return off;
}

std::size_t asyncThreads() {
    static const std::size_t threads = [] {
        const std::size_t cores = std::max(1u, std::thread::hardware_concurrency());
        if (const char* text = std::getenv("GROOMLAB_ASYNC_THREADS")) return static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
        return std::max<std::size_t>(1, cores - cores / 4);
    }();
    return threads;
}
}  // namespace

GroomShape::~GroomShape() {
    if (attrCallback_ != 0) MMessage::removeCallback(attrCallback_);
    if (scalpAttrCallback_ != 0) MMessage::removeCallback(scalpAttrCallback_);
    gPendingCommits.erase(this);  // (a commit still evaluating is waited for by commit_, before the session goes)
    gPendingWarms.erase(this);    // (a warm-up still evaluating likewise, by warm_)
    if (gBrushTarget == this) gBrushTarget = nullptr;
}

void GroomShape::setBrushTarget(GroomShape* shape) {
    if (shape != gBrushTarget) {
        GroomShape* before = gBrushTarget;
        gBrushTarget = shape;
        if (before) before->settleCommit("setBrushTarget");
        if (before) before->takeWarm(true);  // (its document goes below)
        if (before && !before->session_.hasPreview()) {
            // The brush tool put away (or on another groom): its stroke document's memory (a
            // third of the view's at a sculptDensity of 0.35) goes, and the viewport lets the
            // dimmed strands' buffers go.
            before->session_.releaseStroke();
            ++before->displayVersion_;
            MHWRender::MRenderer::setGeometryDrawDirty(before->thisMObject());
        }
    }
    if (shape) shape->scheduleStrokeWarmup();
}

void GroomShape::setSymmetrySeam(std::vector<Vec3f> segments) {
    if (segments.size() == symmetrySeam_.size() && std::equal(segments.begin(), segments.end(), symmetrySeam_.begin(), [](const Vec3f& a, const Vec3f& b) {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        })) {
        return;
    }
    symmetrySeam_ = std::move(segments);
    ++symmetrySeamVersion_;
    M3dView::scheduleRefreshAllViews();
}

void GroomShape::setTubeWires(bool on) {
    if (on == tubeWires_) return;
    tubeWires_ = on;
    M3dView::scheduleRefreshAllViews();
}

void GroomShape::stopStrokeWarmup() {
    if (gWarmCallback != 0) MMessage::removeCallback(gWarmCallback);
    gWarmCallback = 0;
    if (gAdoptCallback != 0) MMessage::removeCallback(gAdoptCallback);
    gAdoptCallback = 0;
    gPendingWarms.clear();
    gBrushTarget = nullptr;
}

bool GroomShape::strokeKept() const { return gBrushTarget == this; }

void GroomShape::scheduleStrokeWarmup() {
    // One idle callback at a time, removed when it ran (an idle callback left registered would
    // keep Maya's idle loop busy); it warms whichever groom the tool is on by then.
    if (strokeWarmupOff() || this != gBrushTarget || gWarmCallback != 0 || !requested_ || hasPreview() || commitPending_) return;
    MStatus status;
    gWarmCallback = MEventMessage::addEventCallback("idle", warmStrokeOnIdle, nullptr, &status);
    if (!status) gWarmCallback = 0;
}

void GroomShape::warmStrokeOnIdle(void*) {
    if (gWarmCallback != 0) MMessage::removeCallback(gWarmCallback);
    gWarmCallback = 0;
    GroomShape* shape = gBrushTarget;
    // (A commit evaluating: the session is the worker's; its landing schedules the warm-up again.)
    if (!shape || !shape->requested_ || shape->hasPreview() || shape->commitPending_) return;
    // With what the groom's last evaluation bound (an input edited since and not evaluated yet
    // warms with the old one: its own evaluation schedules this again). Nothing is thrown here.
    try {
        shape->startWarm();
    } catch (...) {
    }
}

void GroomShape::startWarm() {
    // GROOMLAB_ASYNC_WARM=0 in the environment: on the UI thread (comparisons).
    static const bool async = [] {
        const char* value = std::getenv("GROOMLAB_ASYNC_WARM");
        return !(value && std::string(value) == "0");
    }();
    if (!async) {
        session_.warmStroke(lastRequest_);
        return;
    }
    if (warm_.pending()) {  // (one at a time: this one once that one is in)
        warmAgain_ = true;
        return;
    }
    std::shared_ptr<DetachedEvaluation> run = session_.detachWarm(lastRequest_);
    if (!run) return;
    warm_.setThreads(asyncThreads());
    try {
        warm_.start(std::move(run));
    } catch (const std::exception&) {
        session_.warmStroke(lastRequest_);  // no thread: warmed here
        return;
    }
    gPendingWarms.insert(this);
    if (gAdoptCallback == 0) {
        MStatus status;
        gAdoptCallback = MEventMessage::addEventCallback("idle", adoptWarmsOnIdle, nullptr, &status);
        if (!status) gAdoptCallback = 0;
    }
}

void GroomShape::takeWarm(bool wait) {
    if (!warm_.pending() || (!wait && !warm_.ready())) return;
    std::shared_ptr<DetachedEvaluation> run = warm_.wait();
    gPendingWarms.erase(this);
    // Refused when the stroke document moved on meanwhile (a press evaluated it, another graph).
    if (run) session_.adoptWarm(*run);
    run.reset();  // (the document it replaced goes with it)
    if (warmAgain_) {
        warmAgain_ = false;
        scheduleStrokeWarmup();
    }
}

void GroomShape::adoptWarmsOnIdle(void*) {
    // (Removed only here, by itself, once no warm-up is pending.)
    std::vector<GroomShape*> pending(gPendingWarms.begin(), gPendingWarms.end());
    for (GroomShape* shape : pending) {
        if (!shape->warm_.pending()) {
            gPendingWarms.erase(shape);
            continue;
        }
        // Run, and the session not mid-evaluation (a compute taking a commit in).
        if (!shape->warm_.ready() || shape->computing_) continue;
        try {
            shape->takeWarm(false);
        } catch (...) {
            gPendingWarms.erase(shape);
        }
    }
    if (gPendingWarms.empty() && gAdoptCallback != 0) {
        MMessage::removeCallback(gAdoptCallback);
        gAdoptCallback = 0;
    }
}

void GroomShape::postConstructor() {
    MPxSurfaceShape::postConstructor();
    // A control attribute added, removed or renamed (a Control node, a keyed param, undo of
    // either): the list of them is found again on the next compute.
    MStatus status;
    MObject self = thisMObject();
    attrCallback_ = MNodeMessage::addAttributeChangedCallback(
        self,
        [](MNodeMessage::AttributeMessage msg, MPlug&, MPlug&, void* data) {
            if (msg & (MNodeMessage::kAttributeAdded | MNodeMessage::kAttributeRemoved | MNodeMessage::kAttributeRenamed)) {
                static_cast<GroomShape*>(data)->live_.controlsKnown = false;
            }
        },
        this, &status);
    if (!status) attrCallback_ = 0;
}

std::shared_ptr<const GroomShape::NodeLinks> GroomShape::links() {
    std::lock_guard<std::mutex> lock(linksMutex_);
    return links_;
}

const MObject* GroomShape::textAttribute(int input) {
    switch (input) {
        case kGraphText: return &aGraph;
        case kGuideText: return &aGuideData;
        case kScalpMapsText: return &aScalpMaps;
        case kCorrectiveText: return &aCorrectiveData;
        case kPartText: return &aPartData;
        case kFlowText: return &aFlowData;
        case kSculptText: return &aSculptData;
        default: return nullptr;
    }
}

bool GroomShape::takeTextDirty(int input) {
    const bool read = textDirty_[input] || MPlug(thisMObject(), *textAttribute(input)).isDestination();
    textDirty_[input] = false;
    return read;
}

void GroomShape::markDirty(const MPlug& plug) {
    if (plug == aInMesh || plug == aRestMesh) live_.meshDirty = true;
    if (plug == aColliderMeshes || plug == aColliderMesh || plug == aColliderName || plug == aColliderRestMesh) live_.collidersDirty = true;
    if (plug == aExtraScalps || plug == aExtraScalpMesh || plug == aExtraScalpName || plug == aExtraScalpRestMesh) live_.scalpsDirty = true;
    if (plug == aGuideCurves) live_.curvesDirty = true;
    for (int i = 0; i < kTextInputs; ++i) {
        if (plug == *textAttribute(i)) textDirty_[i] = true;
    }
}

MStatus GroomShape::preEvaluation(const MDGContext& context, const MEvaluationNode& evaluationNode) {
    if (context.isNormal()) {
        for (const MObject* attr : {&aInMesh, &aRestMesh, &aColliderMeshes, &aColliderMesh, &aColliderName, &aColliderRestMesh,
                                    &aExtraScalps, &aExtraScalpMesh, &aExtraScalpName, &aExtraScalpRestMesh, &aGuideCurves}) {
            MStatus status;
            if (evaluationNode.dirtyPlugExists(*attr, &status) && status) markDirty(MPlug(thisMObject(), *attr));
        }
        for (int i = 0; i < kTextInputs; ++i) {
            MStatus status;
            if (evaluationNode.dirtyPlugExists(*textAttribute(i), &status) && status) textDirty_[i] = true;
        }
    }
    return MPxSurfaceShape::preEvaluation(context, evaluationNode);
}

std::map<std::string, float> GroomShape::readControlValues(MDataBlock& data, Lane& lane, const NodeLinks* links) {
    if (!links) {
        const MObject self = thisMObject();
        const unsigned count = MFnDependencyNode(self).attributeCount();
        if (!lane.controlsKnown || count != lane.controlCount) {
            lane.controls = controlAttributes(self);
            lane.controlCount = count;
            lane.controlsKnown = true;
        }
    }
    // From the data block: the values in the evaluation's context (a key at another frame).
    std::map<std::string, float> controls;
    const std::size_t prefix = std::strlen(kControlPrefix);
    for (const MObject& attribute : links ? links->controls : lane.controls) {
        MStatus status;
        MDataHandle handle = data.inputValue(attribute, &status);
        if (!status) continue;
        controls[std::string(MFnAttribute(attribute).name().asChar()).substr(prefix)] = handleAsFloat(handle);
    }
    return controls;
}

MStatus GroomShape::initialize() {
    MStatus status;
    MFnTypedAttribute tAttr;
    MFnNumericAttribute nAttr;
    MFnUnitAttribute uAttr;

    // Short names carry a "gl" prefix: plain ones (e.g. "pd") collide with
    // attributes inherited from the shape base class and fail addAttribute.
    aInMesh = tAttr.create("inMesh", "glin", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    tAttr.setHidden(true);

    aRestMesh = tAttr.create("restMesh", "glrm", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    tAttr.setHidden(true);
    // Follow the scalp: the groom evaluated on the rest mesh, each strand carried to the scalp by
    // its root's frame (the rest evaluation is kept while only the scalp moves).
    aFollowScalp = nAttr.create("followScalp", "glfsc", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    nAttr.setNiceNameOverride("Follow Scalp");
    // The scalp's fingerprint (ScalpMesh::topologyFingerprint, groom_file.h text) an imported groom
    // was made on: a scalp of another topology is said in lastError (GroomSession). "": not known.
    aScalpTopology = tAttr.create("scalpTopology", "glstp", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    // Scale with the scalp (GroomSession::setScaleSource): what the groom's scale follows, Surface
    // Area for new grooms and grooms saved before it (their first evaluation is their 1x: nothing
    // changes until the scalp does).
    MFnEnumAttribute sourceAttr;
    aScaleSource = sourceAttr.create("scaleSource", "glssc", 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    sourceAttr.addField("Surface Area", 0);
    sourceAttr.addField("Transform Only", 1);
    sourceAttr.addField("Off", 2);
    sourceAttr.setStorable(true);
    sourceAttr.setKeyable(false);
    sourceAttr.setNiceNameOverride("Scale Source");
    // The transform scale the groom is 1x at (Transform Only); 0: the first one measured.
    aScaleTransformReference = nAttr.create("scaleTransformReference", "glstr", MFnNumericData::kDouble, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    nAttr.setMin(0.0);
    nAttr.setHidden(true);
    // The scalp area the groom is 1x at (scene units squared); 0: the first area measured.
    aScaleReference = nAttr.create("scaleReference", "glsrf", MFnNumericData::kDouble, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    nAttr.setMin(0.0);
    nAttr.setHidden(true);
    aGroomScale = nAttr.create("groomScale", "glgsc", MFnNumericData::kDouble, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setWritable(false);
    nAttr.setStorable(false);
    nAttr.setNiceNameOverride("Groom Scale");
    aScaleArea = nAttr.create("scaleArea", "glsar", MFnNumericData::kDouble, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setWritable(false);
    nAttr.setStorable(false);
    nAttr.setHidden(true);
    aScaleTransform = nAttr.create("scaleTransform", "glstm", MFnNumericData::kDouble, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setWritable(false);
    nAttr.setStorable(false);
    nAttr.setHidden(true);

    // Colliders: meshes the graph reads as "mesh:<name>" (ColliderMesh nodes), for Collide nodes.
    aColliderMesh = tAttr.create("colliderMesh", "glclm", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    aColliderName = tAttr.create("colliderName", "glcln", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    // Its rest pose (optional): the same mesh undeformed, read only while connected.
    aColliderRestMesh = tAttr.create("colliderRestMesh", "glclr", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    MFnCompoundAttribute colliderAttr;
    aColliderMeshes = colliderAttr.create("colliderMeshes", "glcol", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    colliderAttr.addChild(aColliderMesh);
    colliderAttr.addChild(aColliderName);
    colliderAttr.addChild(aColliderRestMesh);
    colliderAttr.setArray(true);
    colliderAttr.setHidden(true);
    colliderAttr.setDisconnectBehavior(MFnAttribute::kDelete);

    // The groom's other scalps (several scalps on one groom): joined after inMesh, each read by
    // the Scalp nodes naming it.
    aExtraScalpMesh = tAttr.create("extraScalpMesh", "glxsm", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    aExtraScalpName = tAttr.create("extraScalpName", "glxsn", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aExtraScalpRestMesh = tAttr.create("extraScalpRestMesh", "glxsr", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    MFnCompoundAttribute scalpsAttr;
    aExtraScalps = scalpsAttr.create("extraScalps", "glxs", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    scalpsAttr.addChild(aExtraScalpMesh);
    scalpsAttr.addChild(aExtraScalpName);
    scalpsAttr.addChild(aExtraScalpRestMesh);
    scalpsAttr.setArray(true);
    scalpsAttr.setHidden(true);
    scalpsAttr.setDisconnectBehavior(MFnAttribute::kDelete);

    // Guide poses: simulated curves by guide ID, and an Alembic guide cache.
    aSimCurve = tAttr.create("simCurve", "glsmc", MFnData::kNurbsCurve, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(false);
    // Guide IDs are 64-bit (generated children hash theirs): stored as decimal text.
    aSimGuideId = tAttr.create("simGuideId", "glsmi", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    MFnCompoundAttribute cAttr;
    aSimGuides = cAttr.create("simGuides", "glsmg", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    cAttr.addChild(aSimCurve);
    cAttr.addChild(aSimGuideId);
    cAttr.setArray(true);
    cAttr.setHidden(true);
    cAttr.setDisconnectBehavior(MFnAttribute::kDelete);

    aGuideCache = tAttr.create("guideCache", "glgch", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setUsedAsFilename(true);
    aGuideCacheObject = tAttr.create("guideCacheObject", "glgco", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    // The Simulate nodes' bakes (groomLabSimulate -bake, groomlab.sim.bake_simulation): their frames
    // are read from these files while the nodes' settings are the ones baked.
    aSimCaches = tAttr.create("simCaches", "glsbk", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    // The marker pairs of the groom's last transfer by markers (groomLabTransfer -markers: a root
    // and a world point per side, transfer.h transferMarkersToJson), reused by the next transfer
    // and written into .groom files. Affects no evaluation.
    aTransferMarkers = tAttr.create("transferMarkers", "gltmk", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);

    aGuideCurves = tAttr.create("guideCurves", "glgc", MFnData::kNurbsCurve, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setArray(true);
    tAttr.setReadable(false);  // indexMatters(false), needed for connectAttr -nextAvailable, requires this
    tAttr.setIndexMatters(false);
    tAttr.setStorable(false);
    tAttr.setHidden(true);
    tAttr.setDisconnectBehavior(MFnAttribute::kDelete);

    aTime = uAttr.create("time", "gltm", MFnUnitAttribute::kTime, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    uAttr.setStorable(false);

    MFnStringData stringData;
    aGraph = tAttr.create("graph", "glgr", MFnData::kString, stringData.create("{}"), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);

    aPartData = tAttr.create("partData", "glpt", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);

    aGuideData = tAttr.create("guideData", "glgd", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    aFlowData = tAttr.create("flowData", "glfld", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    aDisplayFlow = nAttr.create("displayFlow", "gldfl", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aCorrectiveData = tAttr.create("correctiveData", "glcrd", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    // Corrective layer weights, keyable (the layers' names are aliases): fades are keys.
    aCorrectiveWeight = nAttr.create("correctiveWeight", "glcw", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setArray(true);
    nAttr.setUsesArrayDataBuilder(true);
    nAttr.setKeyable(true);
    nAttr.setStorable(true);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(1.0);
    aSculptData = tAttr.create("sculptData", "glscd", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    aSculptNode = tAttr.create("sculptNode", "glsknd", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aShotLayer = tAttr.create("shotLayer", "glshl", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aShotSculpt = nAttr.create("shotSculpt", "glshs", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);

    aDisplayInstances = nAttr.create("displayInstances", "gldi", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setChannelBox(true);
    aLocked = nAttr.create("locked", "gllk", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setChannelBox(true);
    aViewNode = tAttr.create("viewNode", "glvn", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aGraphUi = tAttr.create("graphUi", "glgu", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);

    // Guide selection (GT-5): drawn highlighted, and a mask for brushes. Doesn't affect evaluation.
    MFnStringArrayData stringArrayData;
    aGuideSelection = tAttr.create("guideSelection", "glgs", MFnData::kStringArray, stringArrayData.create(), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    aSoftSelectRadius = nAttr.create("softSelectRadius", "glssr", MFnNumericData::kFloat, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(2.0);
    nAttr.setStorable(true);
    nAttr.setChannelBox(true);

    // Viewport look (VP-1, VP-3): redraws only, never re-evaluates.
    MFnEnumAttribute eAttr;
    aStrandColorMode = eAttr.create("strandColorMode", "glscm", 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("solid", 0);
    eAttr.addField("colorSet", 1);
    eAttr.addField("attribute", 2);
    eAttr.addField("random", 3);
    eAttr.addField("clump", 4);
    eAttr.addField("density", 5);
    eAttr.setStorable(true);
    eAttr.setChannelBox(true);
    aStrandColorSource = tAttr.create("strandColorSource", "glscs", MFnData::kString, stringData.create("clumpId"), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aStrandColorMin = nAttr.create("strandColorMin", "glscn", MFnNumericData::kFloat, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aStrandColorMax = nAttr.create("strandColorMax", "glscx", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayRibbons = nAttr.create("displayRibbons", "gldr", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setChannelBox(true);
    aRibbonWidthScale = nAttr.create("ribbonWidthScale", "glrws", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(10.0);
    nAttr.setStorable(true);
    nAttr.setChannelBox(true);
    aDisplayWidthCoverage = nAttr.create("displayWidthCoverage", "gldwc", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayCoverageDensity = nAttr.create("displayCoverageDensity", "glcvd", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayClumpLod = nAttr.create("displayClumpLod", "glclo", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aClumpLodError = nAttr.create("clumpLodError", "glcle", MFnNumericData::kFloat, StrandLodSettings{}.error, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.01);
    nAttr.setMax(0.45);
    nAttr.setStorable(true);
    struct RenderAttr {
        MObject* attr;
        const char* name;
        const char* shortName;
        MFnNumericData::Type type;
        double value, softMax;
    };
    for (const RenderAttr& a : {RenderAttr{&aLodDistance, "lodDistance", "gllod", MFnNumericData::kBoolean, 0.0, 1.0},
                                RenderAttr{&aLodNear, "lodNear", "gllnr", MFnNumericData::kFloat, 10.0, 100.0},
                                RenderAttr{&aLodFar, "lodFar", "gllfr", MFnNumericData::kFloat, 100.0, 1000.0},
                                RenderAttr{&aLodMinDensity, "lodMinDensity", "gllmd", MFnNumericData::kFloat, 0.1, 1.0},
                                RenderAttr{&aLodWidthCompensation, "lodWidthCompensation", "gllwc", MFnNumericData::kFloat, 1.0, 1.0},
                                RenderAttr{&aCameraCull, "cameraCull", "glccl", MFnNumericData::kBoolean, 0.0, 1.0},
                                RenderAttr{&aCullMargin, "cullMargin", "glcmg", MFnNumericData::kFloat, 0.1, 1.0},
                                RenderAttr{&aBackfaceCull, "backfaceCull", "glbfc", MFnNumericData::kBoolean, 0.0, 1.0},
                                RenderAttr{&aBackfaceAngle, "backfaceAngle", "glbfa", MFnNumericData::kFloat, 20.0, 90.0}}) {
        *a.attr = nAttr.create(a.name, a.shortName, a.type, a.value, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        nAttr.setStorable(true);
        if (a.type == MFnNumericData::kFloat) {
            nAttr.setMin(0.0);
            nAttr.setSoftMax(a.softMax);
        }
    }
    aDisplayLighting = nAttr.create("displayLighting", "gldlt", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayShadows = eAttr.create("displayShadows", "gldsh", 1, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("Off", 0);
    eAttr.addField("Cast", 1);
    eAttr.addField("Cast and Receive", 2);
    eAttr.setStorable(true);
    aDisplayHairShading = eAttr.create("displayHairShading", "gldhs", 1, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("Maya", 0);
    eAttr.addField("GroomLab", 1);
    eAttr.setStorable(true);
    aDisplaySelfShadowing = nAttr.create("displaySelfShadowing", "gldss", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(2.0);
    nAttr.setStorable(true);
    aDisplayStrandVariation = nAttr.create("displayStrandVariation", "gldsv", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(2.0);
    nAttr.setStorable(true);
    aDisplayColor = nAttr.createColor("displayColor", "gldcl", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setDefault(0.62f, 0.45f, 0.28f);
    nAttr.setStorable(true);
    aUseMaterialColor = nAttr.create("useMaterialColor", "glumc", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);

    aSculptDisplay = eAttr.create("sculptDisplay", "glsd", 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("normal", 0);
    eAttr.addField("dimStrands", 1);
    eAttr.addField("guidesOnly", 2);
    eAttr.setStorable(true);
    aSculptDensity = nAttr.create("sculptDensity", "glsdn", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.01);
    nAttr.setMax(1.0);
    nAttr.setStorable(true);
    // A guide group at full color with its child groups, the others dimmed ("": none; Isolate).
    aIsolateGroup = tAttr.create("isolateGroup", "glisg", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    // Each guide group's own color (theirs, else an automatic one per group), or the painted layer
    // guideColorLayer from black to white.
    aGuideColorMode = eAttr.create("guideColorMode", "glgcm", 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("Group", 0);
    eAttr.addField("Painted Layer", 1);
    eAttr.setNiceNameOverride("Guide Color");
    eAttr.setStorable(true);
    aGuideColorLayer = tAttr.create("guideColorLayer", "glgcl", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    // Guide groups hidden in the viewport (names, space separated): not drawn, brushed or selected.
    // Guide group colors (name:#rrggbb, space separated, sRGB): a group's own color in the viewport.
    aGuideGroupColors = tAttr.create("guideGroupColors", "glggc", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aHiddenGuideGroups = tAttr.create("hiddenGuideGroups", "glhgg", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aGuideWidth = nAttr.create("guideWidth", "glgdw", MFnNumericData::kFloat, 4.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(1.0);
    nAttr.setSoftMax(16.0);
    nAttr.setStorable(true);
    aGuidePoints = eAttr.create("guidePoints", "glgpt", 2, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("off", 0);
    eAttr.addField("roots and tips", 1);
    eAttr.addField("all", 2);  // thinned out on grooms with many guides
    eAttr.setStorable(true);
    // Guides draw as a smooth curve through their points: this many pieces per segment (1: straight segments).
    aGuideSmoothing = nAttr.create("guideSmoothing", "glgsm", MFnNumericData::kInt, 4, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(1);
    nAttr.setSoftMax(8);
    nAttr.setMax(16);
    nAttr.setStorable(true);

    // Render settings read by the Arnold translator; they don't affect evaluation.
    aRenderMode = eAttr.create("renderMode", "glrdm", 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    eAttr.addField("curves", 0);
    eAttr.addField("groomFile", 1);
    eAttr.setStorable(true);
    eAttr.setKeyable(false);

    aGroomFile = tAttr.create("groomFile", "glgf", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setUsedAsFilename(true);

    // Render overrides (renders alone, never the viewport): 1 changes nothing.
    aRenderDensity = nAttr.create("renderDensity", "glrdd", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setMax(1.0);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    aRenderWidthScale = nAttr.create("renderWidthScale", "glrdw", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(4.0);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    aRenderLengthScale = nAttr.create("renderLengthScale", "glrdl", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(4.0);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    aRenderView = nAttr.create("renderView", "glrvw", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(false);
    nAttr.setHidden(true);
    nAttr.setKeyable(false);

    aDisplayGuides = nAttr.create("displayGuides", "gldg", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setKeyable(false);
    nAttr.setChannelBox(true);

    aDisplayTubes = nAttr.create("displayTubes", "gldt", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setKeyable(false);
    nAttr.setChannelBox(true);

    // The hair in the viewport (the Groom Tools' eye): off, the strands, instances, scales, cards
    // and strand meshes are not drawn; the guides and overlays are. Renders and exports ignore it.
    aDisplayHair = nAttr.create("displayHair", "gldhr", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setKeyable(false);
    nAttr.setChannelBox(true);

    aBrushTick = nAttr.create("brushTick", "glbt", MFnNumericData::kInt, 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(false);
    nAttr.setHidden(true);

    aPreviewDensity = nAttr.create("previewDensity", "glpd", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setMax(1.0);
    nAttr.setKeyable(true);
    // Millions of strand points the viewport may hold (each node keeps a copy, and the
    // draw buffers more): past it the preview thins itself. 0 (the default): no limit.
    aPreviewPointBudget = nAttr.create("previewPointBudget", "glppb", MFnNumericData::kFloat, 0.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.0);
    nAttr.setSoftMax(64.0);
    nAttr.setMax(128.0);
    nAttr.setNiceNameOverride("Point Budget (M)");  // (the Attribute Editor's label column cut a longer one at 125% UI scale)
    nAttr.setStorable(true);

    // Cards are built from the whole groom (1.0): a card carries every strand of its clump.
    aCardDensity = nAttr.create("cardDensity", "glcd", MFnNumericData::kFloat, 1.0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setMin(0.01);
    nAttr.setMax(1.0);
    nAttr.setStorable(true);
    aCardMaps = tAttr.create("cardMaps", "glcmp", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    // Maps painted on the scalp (Groom Tools > Paint; scalp_maps.h), and the one the viewport shows there.
    aScalpMaps = tAttr.create("scalpMaps", "glsmp", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setHidden(true);
    aScalpMapDisplay = tAttr.create("scalpMapDisplay", "glsmd", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aScalpMapChannel = tAttr.create("scalpMapChannel", "glsmch", MFnData::kString, stringData.create("rgb"), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aDisplayCards = nAttr.create("displayCards", "gldcd", MFnNumericData::kBoolean, false, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayMesh = nAttr.create("displayMesh", "gldsm", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aDisplayOverlaysOnTop = nAttr.create("displayOverlaysOnTop", "gloot", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    nAttr.setKeyable(false);
    aDisplayCardTextures = nAttr.create("displayCardTextures", "gldct", MFnNumericData::kBoolean, true, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setStorable(true);
    aOutCards = tAttr.create("outCards", "glocd", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setWritable(false);
    tAttr.setStorable(false);

    aOutRachis = tAttr.create("outRachis", "glorc", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setWritable(false);
    tAttr.setStorable(false);

    aOutMesh = tAttr.create("outMesh", "glosm", MFnData::kMesh, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setWritable(false);
    tAttr.setStorable(false);

    aOutStrandCount = nAttr.create("outStrandCount", "glosc", MFnNumericData::kInt, 0, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    nAttr.setWritable(false);
    nAttr.setStorable(false);

    aLastError = tAttr.create("lastError", "glerr", MFnData::kString, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setWritable(false);
    tAttr.setStorable(false);

    // What the viewport draws, made with outStrandCount: what cached playback keeps of the groom.
    aOutFrame = tAttr.create("outFrame", "glofr", GroomFrameData::kTypeId, MObject::kNullObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setWritable(false);
    tAttr.setStorable(false);
    tAttr.setHidden(true);

    for (MObject* attr : {&aInMesh, &aRestMesh, &aFollowScalp, &aScalpTopology, &aScaleSource, &aScaleReference, &aScaleTransformReference, &aGroomScale, &aScaleArea, &aScaleTransform, &aColliderMeshes, &aExtraScalps, &aRenderDensity, &aRenderWidthScale, &aRenderLengthScale, &aRenderView, &aSimGuides, &aGuideCache, &aGuideCacheObject, &aSimCaches, &aTransferMarkers, &aGuideCurves, &aTime, &aGraph, &aGuideData, &aPartData, &aGraphUi, &aViewNode, &aLocked, &aDisplayInstances, &aGuideSelection, &aSoftSelectRadius, &aStrandColorMode, &aStrandColorSource, &aStrandColorMin, &aStrandColorMax, &aDisplayRibbons, &aRibbonWidthScale, &aDisplayWidthCoverage, &aDisplayCoverageDensity, &aDisplayClumpLod, &aClumpLodError, &aDisplayLighting, &aDisplayShadows, &aDisplayHairShading, &aDisplaySelfShadowing, &aDisplayStrandVariation, &aGuideWidth, &aCorrectiveData, &aCorrectiveWeight, &aSculptData, &aSculptNode, &aFlowData, &aDisplayFlow, &aShotSculpt, &aShotLayer, &aLodDistance, &aLodNear, &aLodFar, &aLodMinDensity,
                          &aLodWidthCompensation, &aCameraCull, &aCullMargin, &aBackfaceCull, &aBackfaceAngle, &aDisplayColor, &aUseMaterialColor, &aSculptDisplay, &aSculptDensity, &aIsolateGroup, &aGuideColorMode, &aGuideColorLayer, &aGuidePoints, &aGuideSmoothing, &aHiddenGuideGroups, &aGuideGroupColors, &aRenderMode, &aGroomFile, &aPreviewDensity, &aPreviewPointBudget, &aDisplayGuides, &aDisplayHair,
                          &aDisplayTubes, &aBrushTick, &aCardDensity, &aOutCards, &aOutRachis, &aOutMesh, &aDisplayCards, &aDisplayMesh, &aDisplayCardTextures, &aDisplayOverlaysOnTop, &aCardMaps, &aScalpMaps, &aScalpMapDisplay, &aScalpMapChannel, &aOutStrandCount, &aLastError, &aOutFrame}) {
        CHECK_MSTATUS_AND_RETURN_IT(addAttribute(*attr));
    }
    for (MObject* input : {&aInMesh, &aRestMesh, &aFollowScalp, &aScalpTopology, &aScaleSource, &aScaleReference, &aScaleTransformReference, &aColliderMeshes, &aColliderMesh, &aColliderName, &aColliderRestMesh,
                           &aExtraScalps, &aExtraScalpMesh, &aExtraScalpName, &aExtraScalpRestMesh, &aSimGuides, &aSimCurve, &aSimGuideId, &aGuideCache, &aGuideCacheObject, &aSimCaches, &aGuideCurves, &aTime, &aGraph, &aGuideData, &aCorrectiveData, &aCorrectiveWeight, &aSculptData, &aSculptNode, &aFlowData, &aPartData, &aPreviewDensity, &aPreviewPointBudget, &aRenderView, &aSculptDensity, &aViewNode, &aBrushTick, &aCardDensity, &aDisplayCards, &aDisplayMesh, &aCardMaps, &aScalpMaps}) {
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aOutStrandCount));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aLastError));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aOutCards));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aOutRachis));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aOutMesh));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aOutFrame));
    }
    for (MObject* input : {&aInMesh, &aRestMesh, &aScaleSource, &aScaleReference, &aScaleTransformReference, &aTime}) {
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aGroomScale));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aScaleArea));
        CHECK_MSTATUS_AND_RETURN_IT(attributeAffects(*input, aScaleTransform));
    }
    return MS::kSuccess;
}

void GroomShape::updateGraph(GroomSession& session, const std::string& text, bool reportIt) {
    // Parsed once by the session: its card, rachis, rest and scratch documents are clones that
    // adopt their old ones' caches. The same text again (one that failed too) does nothing.
    try {
        session.setGraphJson(text);
    } catch (const std::exception& e) {
        session.dropGraph(text, std::string("graph: ") + e.what());
        if (reportIt) report(e.what(), true);
    }
}

MeshPtr GroomShape::scaleScalp(Lane& lane, MDataBlock& data, const MeshPtr& scalp, bool meshTouched) {
    if (!scalp || data.inputValue(aScaleSource).asShort() == 2) return scalp;
    if (lane.live) {
        // The scalp's world area at the start frame: on a rig scaled at its root, the bound mesh's
        // (its original shape stays as it was), and one area on every frame of a deforming scalp.
        // At the start frame the scalp as it is; elsewhere read at the start frame when the scalp
        // changed (an edit: a scale, frozen transforms, another mesh; or another frame of it), on
        // the first evaluation or for another start frame. Not while Maya plays or scrubs: the
        // area stays (an edit is made with the time standing).
        const MTime start = MAnimControl::animationStartTime();
        const MTime now = data.inputValue(aTime).asTime();
        double area = 0.0;
        // With a rest mesh, the area without the pose's stretch (poseFreeArea): posing a rig at the
        // start frame keeps the groom's scale; scaling the rig still scales it.
        if (now == start) {
            area = poseFreeArea(*scalp, scalp->points());
        } else if (!(scaleMeasure_.load() > 0.0) || start != scaleMeasuredAt_ || meshTouched) {
            if (!MAnimControl::isPlaying() && !MAnimControl::isScrubbing()) {
                const std::vector<Vec3f> points = scalpPointsAt(thisMObject(), start);
                if (points.size() == scalp->pointCount()) area = poseFreeArea(*scalp, points);
            }
            if (!(area > 0.0) && !(scaleMeasure_.load() > 0.0)) area = poseFreeArea(*scalp, scalp->points());
        }
        if (area > 0.0) {
            scaleMeasure_ = area;
            scaleMeasuredAt_ = start;
        }
    }
    // (The background lane, before any live evaluation: the scalp as it is.)
    const double measure = scaleMeasure_.load() > 0.0 ? scaleMeasure_.load() : surfaceArea(scalp->points(), scalp->triangles());
    return withRestArea(scalp, measure);
}

std::shared_ptr<const GuideHierarchy> GroomShape::storedHierarchy(std::string* error) {
    settleCommit("storedHierarchy");
    session_.setHierarchyJson(MPlug(thisMObject(), aGuideData).asString().asChar());
    if (error) *error = session_.storedHierarchy() ? std::string() : session_.hierarchyError();
    return session_.storedHierarchy();
}

void GroomShape::getCacheSetup(const MEvaluationNode& evalNode, MNodeCacheDisablingInfo& disablingInfo,
                               MNodeCacheSetupInfo& cacheSetupInfo, MObjectArray& monitoredAttributes) const {
    MPxSurfaceShape::getCacheSetup(evalNode, disablingInfo, cacheSetupInfo, monitoredAttributes);
    // Cached frame by frame (in the background too, in any order) when no frame depends on the
    // ones before it; else left out of the cache and evaluated live as it plays. Disabling the
    // cache instead (disablingInfo) would stop cached playback for the whole scene: safe mode.
    const bool cacheable = !const_cast<GroomShape*>(this)->readsEarlierFrames();
    cacheSetupInfo.setPreference(MNodeCacheSetupInfo::kWantToCacheByDefault, cacheable);
    // What decides it: the graph (a Simulate or Jiggle node added, removed or bypassed), the bakes
    // and the node viewed.
    for (const MObject* attr : {&aGraph, &aSimCaches, &aViewNode}) monitoredAttributes.append(*attr);
}

void GroomShape::configCache(const MEvaluationNode& evalNode, MCacheSchema& schema) const {
    MPxSurfaceShape::configCache(evalNode, schema);  // the meshes: outCards, outRachis, outMesh
    // The frame the viewport draws, and what is made with it (or a cached frame would evaluate).
    for (const MObject* attr : {&aOutFrame, &aOutStrandCount, &aLastError}) schema.add(*attr);
}

bool GroomShape::readsEarlierFrames() {
    // The graph, bakes and node viewed as stored now (handed to the live session as compute would).
    settleCommit("readsEarlierFrames");
    if (takeTextDirty(kGraphText)) updateGraph(session_, MPlug(thisMObject(), aGraph).asString().asChar(), true);
    prepareSimulation(thisMObject(), session_, live_.simInputs);
    const std::string view = MPlug(thisMObject(), aViewNode).asString().asChar();
    const int first = static_cast<int>(std::floor(MAnimControl::minTime().as(MTime::uiUnit())));
    const int last = static_cast<int>(std::ceil(MAnimControl::maxTime().as(MTime::uiUnit())));
    return session_.readsEarlierFrames(view, first, last);
}

void GroomShape::report(const std::string& message, bool error) {
    if (message == reported_) return;
    reported_ = message;
    const MString text = MString("GroomLab: ") + name() + ": " + message.c_str();
    if (error) {
        MGlobal::displayError(text);
    } else {
        MGlobal::displayWarning(text);
    }
}

MStatus GroomShape::compute(const MPlug& plug, MDataBlock& data) {
    if (plug != aOutStrandCount && plug != aLastError && plug != aOutFrame && plug != aOutCards && plug != aOutRachis &&
        plug != aOutMesh && plug != aGroomScale && plug != aScaleArea && plug != aScaleTransform) {
        return MS::kUnknownParameter;
    }
    // Another context (cached playback filling frames in the background, a query at another
    // time): the background lane, one thread at a time (it may be asked for outStrandCount from
    // within its own outCards compute). The normal context: the live lane.
    std::unique_lock<std::recursive_mutex> lock;
    const bool normal = data.context().isNormal();
    if (!normal) lock = std::unique_lock<std::recursive_mutex>(background_->mutex);
    Lane& lane = normal ? live_ : background_->lane;
    if (plug == aGroomScale || plug == aScaleArea || plug == aScaleTransform) {
        // What the evaluation measured (made first when its outputs are dirty).
        data.inputValue(aOutStrandCount);
        data.outputValue(aGroomScale).setDouble(lane.session.groomScale());
        data.outputValue(aScaleArea).setDouble(lane.session.scaleArea());
        data.setClean(aGroomScale);
        data.setClean(aScaleArea);
        data.outputValue(aScaleTransform).setDouble(lane.session.scaleTransform());
        data.setClean(aScaleTransform);
        return MS::kSuccess;
    }
    // The live lane's compute: a pending commit settled from within it lands into it (it goes on and
    // makes the frame), not into the data block from outside.
    // (The background lane, on other threads, leaves the flag alone.)
    struct Computing {
        bool* flag;
        bool was = false;
        explicit Computing(bool* f) : flag(f) {
            if (flag) was = std::exchange(*flag, true);
        }
        ~Computing() {
            if (flag) *flag = was;
        }
    } computing(normal ? &computing_ : nullptr);
    // Nothing may throw into Maya's evaluation (out of memory, say): the groom
    // drops its strands and says why instead.
    std::string message;
    try {
        if (plug == aOutRachis) return computeRachis(data, lane);
        if (plug == aOutMesh) return computeStrandMesh(data, lane);
        if (plug == aOutCards) return computeCards(data, lane);
        const auto started = std::chrono::steady_clock::now();
        const MStatus status = evaluate(data, lane);
        lane.lastComputeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return status;
    } catch (const std::exception& e) {
        message = std::string("evaluation failed: ") + e.what();
    } catch (...) {
        message = "evaluation failed";
    }
    if (lane.live) report(message, true);
    MTime now;
    data.context().getTime(now);
    try {
        makeFrame(data, lane, nullptr, mayaSeconds(now), message);
    } catch (...) {
    }
    data.setClean(aOutCards);
    data.setClean(aOutRachis);
    data.setClean(aOutMesh);
    return MS::kSuccess;
}

MStatus GroomShape::computeStrands(MDataBlock& data) { return evaluate(data, live_); }

MStatus GroomShape::evaluate(MDataBlock& data, Lane& lane) {
    GroomSession& session = lane.session;
    const bool live = lane.live;
    std::vector<std::string> errors;  // the host's: the rest mesh, the guide cache
    // A commit the GPU preview already draws (expectCommit, consumed here): it may evaluate off the
    // UI thread when what changed since the last evaluation is the text it commits alone (a keyed
    // param's control too: not tracked here), nothing converted again.
    const CommitKind expected = live ? std::exchange(expectedCommit_, CommitKind::None) : CommitKind::None;
    bool offThread = expected != CommitKind::None && lane.frame && requested_ && asyncCommitsEnabled() &&
                     !lane.meshDirty && !lane.curvesDirty && !lane.collidersDirty && !lane.scalpsDirty && !lane.meshReconnected;
    for (int i = 0; offThread && i < kTextInputs; ++i) {
        const bool committed = (expected == CommitKind::Graph && i == kGraphText) || (expected == CommitKind::Guides && i == kGuideText);
        if (textDirty_[i] && !committed) offThread = false;
    }
    // A commit still evaluating off the UI thread: taken in (the session adopts what it made), and
    // this evaluation goes on from it. Not for the dabs of a stroke chained on it (the session's
    // document as it was, the GPU drawing the strands), nor for that stroke's own commit (detached
    // after it: startCommit); either takes it in before evaluating on the CPU (below).
    const bool chainedDab = live && commitPending_ && chain_ == Chain::Armed;
    const bool chainedCommit = live && commitPending_ && offThread && expected == CommitKind::Guides &&
                               pendingKind_ == CommitKind::Guides && chain_ == Chain::None;
    if (live && !chainedDab && !chainedCommit) finishCommit(true, nullptr, "compute");
    // The background lane takes every input as dirtied: its data block holds another time's values.
    if (!live) lane.meshDirty = lane.curvesDirty = lane.collidersDirty = lane.scalpsDirty = true;
    auto readText = [&](int input) { return live ? takeTextDirty(input) : true; };
    // Off the main thread the node is read through its data block alone: what plugs say (the
    // connections, the names, the control attributes, the paths) comes from the live lane's last
    // evaluation (before any, from the plugs). The live lane reads them and hands them on.
    const std::shared_ptr<const NodeLinks> taken = live ? nullptr : links();
    const NodeLinks* links = taken.get();
    auto next = live ? std::make_shared<NodeLinks>() : nullptr;

    // The stored texts are read only when dirtied (or connected): a frame or a brush stamp
    // copies none of them (guideData runs to megabytes on a big groom). The session parses each
    // only when its text changed.
    if (readText(kGraphText)) updateGraph(session, data.inputValue(aGraph).asString().asChar(), live);

    // The scalp. Converting the mesh (triangles, UVs, color sets) costs more than
    // most evaluations, so it happens only when the mesh plugs were dirtied (an
    // edit, a new connection) or the mesh data reads differently (a deforming
    // scalp at another frame, which cached playback moves without dirtying).
    // A brush stamp or a frame of a static groom keeps the lane's rawScalp.
    // A disconnected mesh attribute keeps its last value: use the rest mesh only while connected.
    const bool hasRest = links ? links->restConnected : MPlug(thisMObject(), aRestMesh).isConnected();
    if (next) next->restConnected = hasRest;
    const MObject meshData = data.inputValue(aInMesh).asMesh();
    const MObject restData = hasRest ? data.inputValue(aRestMesh).asMesh() : MObject::kNullObj;
    const std::uint64_t meshKey = meshFingerprint(meshFingerprint(hash64(0x5CA1F), meshData), restData);
    if (!live) {
        // No connection or edit is seen from here: UVs or colors that read differently (another
        // mesh connected, an edit) convert in full, as a connection does live.
        const std::uint64_t attributes = meshAttributesKey(meshAttributesKey(hash64(0xA77B), meshData), restData);
        if (attributes != lane.attributesKey) lane.meshReconnected = true;
        lane.attributesKey = attributes;
    }
    MeshPtr scalp = lane.rawScalp;
    const bool meshTouched = lane.meshDirty || meshKey != lane.meshKey;  // an edit, or another frame of a moving scalp
    // Scale with the scalp: the scalp's transform scale at the start frame (its world matrix's, its
    // rig's root joint's), read live when the scalp changed (not while Maya plays or scrubs) and
    // handed to the session before any of the scalp's new points, so a scaled transform and the area
    // it gives arrive together (Transform Only's k; Surface Area's notice tells them apart).
    if (live && data.inputValue(aScaleSource).asShort() != 2 && (meshTouched || !(scaleTransformMeasure_.load() > 0.0)) &&
        !MAnimControl::isPlaying() && !MAnimControl::isScrubbing()) {
        const double transform = scalpTransformScaleAt(thisMObject(), MAnimControl::animationStartTime());
        if (transform > 0.0) {
            scaleTransformMeasure_ = transform;
            if (!(firstScaleTransform_.load() > 0.0)) firstScaleTransform_ = transform;
        }
    }
    session.setScaleTransform(scaleTransformMeasure_.load());
    const bool scalpReconnected = lane.meshReconnected;  // (another mesh: a transfer sets the scale itself)
    if (meshTouched) {
        // Other points on the topology of the last conversion (a deforming scalp's next frame, a
        // moving transform): only the points are read, the session moves them on the scalp it
        // has (its UVs and color sets kept, its closest-point tree refitted). The same points
        // with the plug dirtied (an edit of the UVs or colors), a new connection or another
        // topology: converted in full.
        bool pointsOnly = false;
        if (lane.rawScalp && !lane.meshReconnected && meshKey != lane.meshKey) {
            const std::uint64_t topology = meshTopologyKey(hash64(0x70B0), meshData);
            // A rest mesh that moved (its transform animates): its points too, on its topology.
            const std::uint64_t restKey = meshPointsKey(hash64(0x2E57), restData);
            const bool restKept = restKey == lane.restKey;
            if (topology == lane.topologyKey && (restKept || lane.restTopologyKey == meshTopologyKey(hash64(0x70B0), restData))) {
                std::vector<Vec3f> points = meshWorldPoints(meshData);
                std::vector<Vec3f> rest = restKept ? std::vector<Vec3f>() : meshWorldPoints(restData);
                // Sized as scaleScalp sizes it (the session's k stays as it was).
                if (!restKept && (data.inputValue(aScaleSource).asShort() != 2) && rest.size() == lane.rawScalp->pointCount()) {
                    scaleToArea(rest, lane.rawScalp->triangles(), scaleMeasure_.load());
                }
                if (points.size() == lane.rawScalp->pointCount() && session.rawScalp() == lane.rawScalp &&
                    (restKept || (lane.rawScalp->hasRest() && rest.size() == lane.rawScalp->pointCount()))) {
                    session.updateScalpPoints(std::move(points), restKept ? nullptr : &rest);
                    scalp = session.rawScalp();
                    lane.restKey = restKey;
                    pointsOnly = true;
                }
            }
        }
        if (!pointsOnly) {
            std::string restError;
            scalp = toScalpMesh(meshData, restData, &restError);
            // The same scalp as last time: keep the old one, with its closest-point tree built.
            if (scalp && lane.rawScalp && scalp->contentHash() == lane.rawScalp->contentHash()) scalp = lane.rawScalp;
            if (restError != lane.restError) {
                lane.restError = restError;
                if (live && !restError.empty()) report(restError, false);  // once per change
            }
            lane.topologyKey = meshTopologyKey(hash64(0x70B0), meshData);
            lane.restTopologyKey = meshTopologyKey(hash64(0x70B0), restData);
            lane.restKey = meshPointsKey(hash64(0x2E57), restData);
        }
        lane.meshKey = meshKey;
        lane.meshDirty = false;
        lane.meshReconnected = false;
    }
    if (!lane.restError.empty()) errors.push_back(lane.restError);
    // Match render (subdivision.h): the mesh's Arnold subdivision or smooth preview, attributes of
    // the mesh node read live only (on the main thread; a background lane takes the last read).
    if (live) scalpRender_.levels = renderSubdivisionOf(MPlug(thisMObject(), aInMesh), &scalpRender_.from);
    scalp = withRenderSubdivision(scalp, scalpRender_.levels, scalpRender_.from);
    // Scale with the scalp: the rest mesh sized to the scalp's area at the start frame, the
    // reference set (or the first area measured) before the scalp, so k is worked out once.
    scalp = scaleScalp(lane, data, scalp, meshTouched);
    const short source = data.inputValue(aScaleSource).asShort();
    session.setScaleSource(source == 1 ? ScaleSource::Transform : source == 2 ? ScaleSource::Off : ScaleSource::Area);
    double reference = data.inputValue(aScaleReference).asDouble();
    if (!(reference > 0.0) && scalp) {
        if (!(firstScaleArea_.load() > 0.0)) firstScaleArea_ = restSurfaceArea(*scalp);
        reference = firstScaleArea_.load();
    }
    session.setScaleReference(reference);
    // The transform scale the groom is 1x at (scaleTransformReference, else the first measured).
    double transformReference = data.inputValue(aScaleTransformReference).asDouble();
    if (!(transformReference > 0.0)) transformReference = firstScaleTransform_.load();
    session.setScaleTransformReference(transformReference);
    const std::uint64_t scalpHash = scalp ? scalp->contentHash() : 0;
    lane.rawScalp = scalp;
    session.setScalp(scalp);
    // The groom scaled because the scalp's own area changed (an edit, a remodel; Surface Area): said
    // once, in the Script Editor and in the view (never a dialog). Not for another mesh connected
    // (a transfer, its undo: the transfer sets the scale it keeps).
    if (live && session.scaleNoticeSerial() != scaleNoticeSaid_) {
        scaleNoticeSaid_ = session.scaleNoticeSerial();
        if (!scalpReconnected) {
            const MString line(session.scaleNotice().c_str());
            MGlobal::displayWarning(line);
            if (MGlobal::mayaState() == MGlobal::kInteractive) {
                MGlobal::executeCommandOnIdle(
                    "inViewMessage -statusMessage \"" + line + "\" -position topCenter -fade -fadeStayTime 4000", false);
            }
        }
    }
    session.setScalpFingerprint(parseScalpFingerprint(data.inputValue(aScalpTopology).asString().asChar()));
    session.setFollowScalp(data.inputValue(aFollowScalp).asBool());  // evaluate at rest, carry to the scalp
    // The vertex maps painted on it (scalpMaps): the session lays them on as color sets.
    if (readText(kScalpMapsText)) session.setScalpMapsJson(data.inputValue(aScalpMaps).asString().asChar());
    // The groom's other scalps (extraScalps), joined after it: converted (like the colliders) only
    // when their plugs were dirtied or their mesh data reads differently.
    bool scalpsTouched = false;  // an edit, or another frame of a moving other scalp
    {
        struct ScalpData {
            std::string name;
            MObject mesh, rest;
        };
        std::vector<ScalpData> scalpData;
        std::vector<RenderLevel> scalpLevels;
        std::uint64_t scalpsKey = hash64(0x5CA195);
        std::set<std::string> named;  // each name once
        MArrayDataHandle handles = data.inputArrayValue(aExtraScalps);
        const MPlug arrayPlug = links ? MPlug() : MPlug(thisMObject(), aExtraScalps);
        for (unsigned i = 0; i < handles.elementCount(); ++i, handles.next()) {
            const unsigned index = handles.elementIndex();
            MDataHandle element = handles.inputValue();
            const std::string given = element.child(aExtraScalpName).asString().asChar();
            std::string name;
            if (links) {
                const auto found = links->scalpNames.find(index);
                if (found == links->scalpNames.end()) continue;  // not connected
                name = given.empty() ? found->second : given;
            } else {
                const MPlug meshPlug = arrayPlug.elementByLogicalIndex(index).child(aExtraScalpMesh);
                if (!meshPlug.isConnected()) continue;  // stale data once disconnected
                name = colliderNameFor(meshPlug, given);
                if (next) next->scalpNames[index] = colliderNameFor(meshPlug, {});
            }
            if (name.empty() || !validScalpName(name) || !named.insert(name).second) continue;
            const bool restConnected = links ? links->scalpRests.count(index) > 0
                                             : arrayPlug.elementByLogicalIndex(index).child(aExtraScalpRestMesh).isConnected();
            if (next && restConnected) next->scalpRests.insert(index);
            scalpData.push_back({name, element.child(aExtraScalpMesh).asMesh(),
                                 restConnected ? element.child(aExtraScalpRestMesh).asMesh() : MObject::kNullObj});
            scalpsKey = meshFingerprint(hashCombine(scalpsKey, std::hash<std::string>{}(name)), scalpData.back().mesh);
            scalpsKey = meshFingerprint(hashCombine(scalpsKey, 0x2E57), scalpData.back().rest);
            RenderLevel& render = scalpsRender_[index];  // match render, read live only
            if (live) render.levels = renderSubdivisionOf(MPlug(thisMObject(), aExtraScalps).elementByLogicalIndex(index).child(aExtraScalpMesh), &render.from);
            scalpLevels.push_back(render);
            scalpsKey = hashCombine(scalpsKey, static_cast<std::uint64_t>(render.levels));
        }
        if (lane.scalpsDirty || scalpsKey != lane.scalpsKey) {
            scalpsTouched = true;
            NamedScalps scalps;
            for (std::size_t s = 0; s < scalpData.size(); ++s) {
                MeshPtr mesh = withRenderSubdivision(toScalpMesh(scalpData[s].mesh, scalpData[s].rest), scalpLevels[s].levels, scalpLevels[s].from);
                if (!mesh || mesh->triangleCount() == 0) continue;
                for (const auto& [oldName, old] : lane.scalps) {  // the same mesh as last time: kept, with its tree built
                    if (oldName == scalpData[s].name && old && old->contentHash() == mesh->contentHash()) mesh = old;
                }
                scalps.emplace_back(scalpData[s].name, std::move(mesh));
            }
            lane.scalps = std::move(scalps);
            lane.scalpsKey = scalpsKey;
            lane.scalpsDirty = false;
        }
    }
    session.setScalps(lane.scalps);
    const MeshPtr joinedScalp = session.joinedScalp();  // what flat guides are rooted on

    // Guide curves: sampled by arc length (slow) only when they changed. Their CVs are
    // fingerprinted only when their plugs were dirtied (an edit, a connection, a frame of an
    // animated curve), and sampled again when that reads differently or the scalp changed.
    std::vector<GuideCurveData> curves;
    MArrayDataHandle curveHandles = data.inputArrayValue(aGuideCurves);
    for (unsigned i = 0; i < curveHandles.elementCount(); ++i, curveHandles.next()) {
        curves.push_back({curveHandles.elementIndex(), curveHandles.inputValue().asNurbsCurve()});
    }
    if (lane.curvesDirty || !lane.guides) {
        std::uint64_t key = hash64(0xC0AE5);
        for (const GuideCurveData& c : curves) key = curveFingerprint(hashCombine(key, c.id), c.curve);
        lane.curvesDataKey = key;
    }
    const std::uint64_t curvesKey = hashCombine(lane.curvesDataKey, joinedScalp == scalp || !joinedScalp ? scalpHash : joinedScalp->contentHash());
    if (!lane.guides || curvesKey != lane.curvesKey) {
        lane.guides = toGuides(curves, joinedScalp.get());
        lane.curvesKey = curvesKey;
    }
    lane.curvesDirty = false;
    session.setGuides(lane.guides);
    // Colliders: converted (like the scalp) only when their plugs were dirtied or
    // their mesh data reads differently (a moving collider at another frame). With a rest mesh
    // connected (the same mesh undeformed), the mesh carries it as its rest pose: Collide and the
    // simulation ask its rest pose's distance field (RestPoseCollision), made once for every frame,
    // where a deforming collider would get a field per frame.
    std::uint64_t collidersKey = hash64(0xC011);
    struct ColliderData {
        std::string name;
        MObject mesh, rest;
    };
    std::vector<ColliderData> colliderData;
    std::vector<RenderLevel> colliderLevels;
    {
        MArrayDataHandle colliderHandles = data.inputArrayValue(aColliderMeshes);
        const MPlug colliderPlug = links ? MPlug() : MPlug(thisMObject(), aColliderMeshes);
        for (unsigned i = 0; i < colliderHandles.elementCount(); ++i, colliderHandles.next()) {
            const unsigned index = colliderHandles.elementIndex();
            MDataHandle element = colliderHandles.inputValue();
            const std::string given = element.child(aColliderName).asString().asChar();
            std::string name;
            if (links) {
                const auto found = links->colliderNames.find(index);
                if (found == links->colliderNames.end()) continue;  // not connected
                name = given.empty() ? found->second : given;
            } else {
                const MPlug meshPlug = colliderPlug.elementByLogicalIndex(index).child(aColliderMesh);
                if (!meshPlug.isConnected()) continue;  // stale data once disconnected
                name = colliderNameFor(meshPlug, given);
                if (next) next->colliderNames[index] = colliderNameFor(meshPlug, {});
            }
            if (name.empty()) continue;
            // A disconnected rest mesh keeps its last value: read only while connected.
            const bool restConnected = links ? links->colliderRests.count(index) > 0
                                             : colliderPlug.elementByLogicalIndex(index).child(aColliderRestMesh).isConnected();
            if (next && restConnected) next->colliderRests.insert(index);
            colliderData.push_back({name, element.child(aColliderMesh).asMesh(),
                                    restConnected ? element.child(aColliderRestMesh).asMesh() : MObject::kNullObj});
            collidersKey = meshFingerprint(hashCombine(collidersKey, std::hash<std::string>{}(name)), colliderData.back().mesh);
            collidersKey = meshFingerprint(hashCombine(collidersKey, 0x2E57), colliderData.back().rest);
            // Match render, as the scalp's (read live only).
            RenderLevel& render = colliderRender_[index];
            if (live) render.levels = renderSubdivisionOf(MPlug(thisMObject(), aColliderMeshes).elementByLogicalIndex(index).child(aColliderMesh), &render.from);
            colliderLevels.push_back(render);
            collidersKey = hashCombine(collidersKey, static_cast<std::uint64_t>(render.levels));
        }
    }
    if (lane.collidersDirty || collidersKey != lane.collidersKey) {
        std::map<std::string, MeshPtr> colliders;
        for (std::size_t c = 0; c < colliderData.size(); ++c) {
            const ColliderData& collider = colliderData[c];
            const std::string& name = collider.name;
            // (A rest mesh of other vertices is left out: the collider as it is, a field per frame.)
            MeshPtr mesh = withRenderSubdivision(toScalpMesh(collider.mesh, collider.rest), colliderLevels[c].levels, colliderLevels[c].from);
            if (!mesh || mesh->triangleCount() == 0) continue;
            auto old = lane.colliders.find(name);  // the same mesh as last time: keep it, with its tree built
            if (old != lane.colliders.end() && old->second && old->second->contentHash() == mesh->contentHash()) mesh = old->second;
            colliders[name] = std::move(mesh);
        }
        lane.colliders = std::move(colliders);
        lane.collidersKey = collidersKey;
        lane.collidersDirty = false;
    }
    session.setMeshes(lane.colliders);  // "mesh:<name>" for ColliderMesh nodes
    if (readText(kGuideText)) session.setHierarchyJson(data.inputValue(aGuideData).asString().asChar());
    data.inputValue(aBrushTick);  // read so the tick counts as an input

    // Guide poses (simulation) on top of the authored hierarchy.
    std::vector<SimCurveData> simCurves;
    MArrayDataHandle simHandles = data.inputArrayValue(aSimGuides);
    const MPlug simPlug = links ? MPlug() : MPlug(thisMObject(), aSimGuides);
    for (unsigned i = 0; i < simHandles.elementCount(); ++i, simHandles.next()) {
        const unsigned index = simHandles.elementIndex();
        const bool connected = links ? links->simCurves.count(index) != 0 : simPlug.elementByLogicalIndex(index).child(0).isConnected();
        if (!connected) continue;  // stale data once disconnected
        if (next) next->simCurves.insert(index);
        MDataHandle element = simHandles.inputValue();
        simCurves.push_back({std::strtoull(element.child(aSimGuideId).asString().asChar(), nullptr, 10),
                             element.child(aSimCurve).asNurbsCurve()});
    }
    std::string poseError;
    const std::shared_ptr<const PathResolver> paths = links ? links->paths : nodePathResolver(thisMObject());
    if (next) next->paths = paths;
    const std::string storedCache = data.inputValue(aGuideCache).asString().asChar();
    std::string cacheFile;
    if (!storedCache.empty()) {
        const ScopedPathResolver scope(paths.get());
        cacheFile = resolvePath(storedCache);
    }
    const std::string cacheObject = data.inputValue(aGuideCacheObject).asString().asChar();
    const MTime now = data.inputValue(aTime).asTime();
    const double seconds = mayaSeconds(now);
    GuidesPtr poses;
    if (simCurves.empty() && !cacheFile.empty()) {
        // A guide cache alone: read it once per time, not on every brush stamp.
        const std::string key = cacheFile + "|" + cacheObject + "|" + std::to_string(seconds);
        if (key != lane.poseKey) {
            lane.posesFromCache = toGuidePoses(simCurves, cacheFile, cacheObject, seconds, &poseError);
            lane.poseKey = poseError.empty() ? key : std::string();
        }
        poses = lane.posesFromCache;
    } else {
        poses = toGuidePoses(simCurves, cacheFile, cacheObject, seconds, &poseError);
    }
    if (!poseError.empty()) {
        errors.push_back(poseError);
        if (live) report(poseError, false);
    }
    session.setGuidePoses(poses);
    // Shot sculpts over the posed groom, at this time: layers with a keyable weight take its
    // value on this frame (the elements of correctiveWeight; a missing one is 1).
    if (readText(kCorrectiveText)) session.setCorrectivesJson(data.inputValue(aCorrectiveData).asString().asChar());
    {
        std::map<int, float> weights;
        MArrayDataHandle handles = data.inputArrayValue(aCorrectiveWeight);
        for (unsigned i = 0; i < handles.elementCount(); ++i, handles.next()) {
            weights[static_cast<int>(handles.elementIndex())] = handles.inputValue().asFloat();
        }
        session.setCorrectiveWeights(std::move(weights));
    }
    // Parts and flow arrows (the session binds them to the scalp; a live stroke's preview instead).
    if (readText(kPartText)) session.setPartsJson(data.inputValue(aPartData).asString().asChar());
    if (readText(kFlowText)) session.setFlowJson(data.inputValue(aFlowData).asString().asChar());
    // Sculpt nodes' edits; live, the Sculpt node brushes shape (its output drawn as the guides).
    if (readText(kSculptText)) session.setSculptsJson(data.inputValue(aSculptData).asString().asChar());
    session.setSculptTarget(live ? std::string(data.inputValue(aSculptNode).asString().asChar()) : std::string());

    // The scalp's earlier frames, only while a Jiggle node makes the output (or the viewed node),
    // and only live (a groom with one is left out of cached playback: getCacheSetup).
    // Frames already seen are reused: playing or scrubbing forward reads one new frame at most
    // (none: the frame before was the current one). Stopped on a frame of a moving scalp (a new
    // time set by a script, an edit to its animation), every frame is read again, as the
    // animation of the frames before may have changed.
    const std::string view = data.inputValue(aViewNode).asString().asChar();
    const int historyFrames = live ? session.scalpHistoryFrames(view) : 0;
    if (live) scalpHistoryFrames_ = historyFrames;
    if (historyFrames > 0 && lane.rawScalp) {
        // Every scalp's earlier frames (the other scalps' after the groom's own), so hair on a
        // moving other scalp jiggles too.
        const bool reread = (meshTouched || scalpsTouched) && !MAnimControl::isPlaying() && !MAnimControl::isScrubbing();
        session.setScalpHistory(sampleScalpHistory(thisMObject(), now, historyFrames, joinedScalpPoints(lane.rawScalp, lane.scalps),
                                                   scalpPast_, reread, session.scalpHistoryLoopFrames(now.as(MTime::uiUnit()), view),
                                                   lane.scalps));
    } else {
        session.setScalpHistory(nullptr);
        if (live && !scalpPast_.empty()) scalpPast_.clear();
    }

    // The controls' values (keys, sliders), from the control attributes found once per change of them.
    session.setControls(readControlValues(data, lane, links));
    if (next) next->controls = lane.controls;
    session.setPathResolver(paths);
    session.setCardMaps(data.inputValue(aCardMaps).asString().asChar());  // the bake the cards keep to
    const bool rachisConnected = links ? links->rachisConnected : MPlug(thisMObject(), aOutRachis).isConnected();
    const bool cardsConnected = links ? links->cardsConnected : MPlug(thisMObject(), aOutCards).isConnected();
    const bool strandMeshConnected = links ? links->strandMeshConnected : MPlug(thisMObject(), aOutMesh).isConnected();
    if (next) {
        next->rachisConnected = rachisConnected;
        next->cardsConnected = cardsConnected;
        next->strandMeshConnected = strandMeshConnected;
        std::lock_guard<std::mutex> lock(linksMutex_);
        links_ = std::move(next);
    }
    if (!rachisConnected) {
        session.releaseFullDensity();  // its full-density cache
        lane.rachisMeshFrom.reset();
        lane.rachisMeshData = MObject::kNullObj;
        lane.rachisMeshMade = false;
    }
    if (!strandMeshConnected) {
        session.releaseStrandMeshes();  // its evaluations' graphs
        lane.strandMeshFrom.clear();
        lane.strandMeshData = MObject::kNullObj;
        lane.strandMeshMade = false;
    }

    // Simulate nodes: the scene's frame rate, the bakes, and live the scalp at the frames a
    // simulation steps over (the background lane never steps: GroomSession::setSimulationStepping).
    const std::string simCaches = data.inputValue(aSimCaches).asString().asChar();
    if (live) {
        prepareSimulation(thisMObject(), session, lane.simInputs);
    } else {
        session.setFramesPerSecond(mayaFramesPerSecond());
        attachSimulationCaches(paths, simCaches, session, lane.simInputs);
    }
    if (!lane.simInputs.error.empty()) {
        errors.push_back(lane.simInputs.error);
        if (live) report(lane.simInputs.error, false);
    }

    ViewRequest request;
    request.density = data.inputValue(aPreviewDensity).asFloat();
    request.sculptDensity = data.inputValue(aSculptDensity).asFloat();  // while a brush stroke is live
    request.pointBudget = 1.0e6 * std::max(0.0f, data.inputValue(aPreviewPointBudget).asFloat());
    request.viewNode = view;  // the graph editor's "view this node"
    request.time = seconds;
    request.viewport = true;  // the viewport's: renderOnly nodes off, viewportOnly ones on
    // Following the scalp, the strands at rest where the viewport carries them on the GPU.
    request.hostCarries = viewportCarries();
    // Drawn as a render shows the groom (renderView, while Hardware 2.0 renders): the render
    // overrides' density, every strand (no point budget), renderOnly nodes on, the strands carried
    // here (makeFrame scales their widths and lengths).
    lane.renderView = data.inputValue(aRenderView).asBool();
    if (lane.renderView) {
        RenderOverrides overrides;
        overrides.density = data.inputValue(aRenderDensity).asFloat();
        request.density = overrides.densityTimes(1.0f);
        request.pointBudget = 0.0;
        request.viewport = false;
        request.hostCarries = false;
        lane.renderWidthScale = std::max(0.0f, data.inputValue(aRenderWidthScale).asFloat());
        lane.renderLengthScale = std::max(0.0f, data.inputValue(aRenderLengthScale).asFloat());
    }
    // Cards: the output at cardDensity, while something uses them: a card mesh on outCards, or
    // Cards nodes drawing in the viewport (mid-stroke only while quick: they catch up after).
    request.wantCards = cardsConnected ||
                        (session.hasCardsNode() && data.inputValue(aDisplayCards).asBool());
    request.cardDensity = data.inputValue(aCardDensity).asFloat();
    // The ToMesh nodes' meshes the viewport draws (displayMesh).
    request.wantStrandMeshes = session.hasStrandMeshNode() && data.inputValue(aDisplayMesh).asBool();
    // A live stroke's dab the GPU preview draws: the guides alone (the hand guides, or a Sculpt
    // node's), unless they are the CPU's.
    if (live) {
        if (!gpu::strokeShapesGuides(session)) gpuStroke_.clear();
        request.stopAt = gpuStroke_;
    }
    // The commit off the UI thread (see expectCommit): the same view as the last evaluation's (the
    // time, densities, node viewed: what the GPU preview drew from), no stroke live, and nothing the
    // evaluation would need Maya for on the worker (a Simulate node steps through the host's feed,
    // the scalp's history is read live) or the cards (outCards, the cards view: built in compute).
    if (offThread && !lane.renderView && !meshTouched && request.stopAt.empty() && !session.hasPreview() && historyFrames == 0 &&
        !request.wantCards && !request.wantStrandMeshes && request.density == lastRequest_.density && request.sculptDensity == lastRequest_.sculptDensity &&
        request.pointBudget == lastRequest_.pointBudget && request.viewNode == lastRequest_.viewNode &&
        request.time == lastRequest_.time && request.hostCarries == lastRequest_.hostCarries &&
        request.cardDensity == lastRequest_.cardDensity && session.simulateNodes().empty() &&
        startCommit(data, request, seconds, errors, expected)) {
        return MS::kSuccess;
    }
    // (On the CPU: a pending commit taken in first, its work reused.)
    if (live && commitPending_ && request.stopAt.empty()) finishCommit(true, nullptr, "compute");
    ++lane.evaluations;
    const ViewResult* evaluated = &session.evaluate(request);
    if (live && !request.stopAt.empty() && !gpuStrokeHolds(*this)) {
        gpuStroke_.clear();
        request.stopAt.clear();
        if (commitPending_) finishCommit(true, nullptr, "compute");
        evaluated = &session.evaluate(request);
    }
    const ViewResult& result = *evaluated;
    std::string joined;
    if (live) {
        joined = finishView(result, request, std::move(errors));
    } else {
        errors.insert(errors.begin(), result.errors.begin(), result.errors.end());
        for (const std::string& e : errors) joined += (joined.empty() ? "" : "\n") + e;
    }
    makeFrame(data, lane, &result, seconds, joined);
    return MS::kSuccess;
}

std::string GroomShape::finishView(const ViewResult& result, const ViewRequest& request, std::vector<std::string> errors) {
    lastRequest_ = request;
    lastRequest_.stopAt.clear();  // (the warm-up evaluates the stroke document whole)
    requested_ = true;
    if (!session_.hasPreview()) scheduleStrokeWarmup();  // while the brush tool is on this groom
    for (const SessionMessage& m : result.messages) report(m.text, m.error);
    // A Scatter making millions of strands (its note, in the Graph Editor too): a warning in the
    // Script Editor, once per change, never a dialog.
    std::string heavy;
    for (const auto& [node, note] : session_.nodeNotes()) {
        if (note.find("strands: heavy") != std::string::npos) heavy += (heavy.empty() ? "" : "; ") + node + ": " + note;
    }
    if (heavy != reportedHeavy_) {
        reportedHeavy_ = heavy;
        if (!heavy.empty()) MGlobal::displayWarning(MString("GroomLab: ") + name() + ": " + heavy.c_str());
    }
    errors.insert(errors.begin(), result.errors.begin(), result.errors.end());
    std::string joined;
    for (const std::string& e : errors) joined += (joined.empty() ? "" : "\n") + e;
    if (errors.empty()) reported_.clear();  // a problem that comes back is reported again
    return joined;
}

bool GroomShape::asyncCommitsEnabled() {
    // GROOMLAB_ASYNC_COMMIT=0 in the environment: every commit evaluates in compute (comparisons).
    static const bool on = [] {
        const char* value = std::getenv("GROOMLAB_ASYNC_COMMIT");
        return !(value && std::string(value) == "0");
    }();
    return on && gpuPreviewEnabled();
}

const GroomSession& GroomShape::session() const {
    const_cast<GroomShape*>(this)->settleCommit("session");
    return session_;
}

void GroomShape::writeFrame(MDataBlock& data, const FramePtr& frame) {
    MFnPluginData fn;
    fn.create(GroomFrameData::kTypeId);
    static_cast<GroomFrameData*>(fn.data())->frame = frame;
    data.outputValue(aOutFrame).set(fn.data());
    const StrandsPtr strands = frame ? frame->view.strands : nullptr;
    data.outputValue(aOutStrandCount).setInt(strands ? static_cast<int>(strands->strandCount()) : 0);
    data.outputValue(aLastError).set(MString(frame ? frame->errors.c_str() : ""));
    data.setClean(aOutFrame);
    data.setClean(aOutStrandCount);
    data.setClean(aLastError);
}

bool GroomShape::startCommit(MDataBlock& data, const ViewRequest& request, double seconds, std::vector<std::string> errors,
                             CommitKind expectedKind) {
    // Its parallel loops on three quarters of the cores (asyncThreads).
    commit_.setThreads(asyncThreads());
    // After a pending one (a stroke chained on it released): going on from what it makes.
    const std::shared_ptr<DetachedEvaluation> run = session_.detach(request, commit_.last());
    if (!run) return false;  // evaluated here (the pending one taken in first)
    try {
        commit_.start(run);
    } catch (const std::exception&) {
        return false;  // no thread: evaluated here
    }
    pendingKind_ = expectedKind;
    commitPending_ = true;
    pendingSeconds_ = seconds;
    pendingErrors_ = std::move(errors);
    pendingRequest_ = request;
    // Until it lands the outputs hold the frame from before it (the very frame: its versions, and
    // with them what the viewport draws, the GPU preview's results, stay).
    writeFrame(data, live_.frame);
    // A stroke's commit: the stroke is over, so is the GPU's part in it (the next stroke arms afresh,
    // even when it starts before the viewport updates again); what its dabs drew stays bound.
    if (GpuLookPreview* preview = gpuLookPreviewFor(this)) preview->endStroke(*this);
    // Landed once evaluated, on Maya's idle (one callback for every groom with a commit pending,
    // registered while there is one: it keeps Maya's idle loop turning for that long).
    gPendingCommits.insert(this);
    if (gLandCallback == 0) {
        MStatus status;
        gLandCallback = MEventMessage::addEventCallback("idle", landCommits, nullptr, &status);
        if (!status) gLandCallback = 0;
    }
    return true;
}

void GroomShape::landCommits(void*) {
    // (Removed only here, by itself, once no commit is pending.)
    std::vector<GroomShape*> pending(gPendingCommits.begin(), gPendingCommits.end());
    for (GroomShape* shape : pending) {
        if (!shape->commitPending_) {
            gPendingCommits.erase(shape);
            continue;
        }
        // Evaluated, and Maya between its own work (not reading or writing a file, not computing it).
        // (Not under a stroke chained on it: its last commit lands after it.)
        if (!shape->commit_.ready() || shape->computing_ || shape->chain_ != Chain::None || MFileIO::isReadingFile() ||
            MFileIO::isWritingFile()) {
            continue;
        }
        shape->finishCommit(false, nullptr, "idle");
    }
    if (gPendingCommits.empty() && gLandCallback != 0) {
        MMessage::removeCallback(gLandCallback);
        gLandCallback = 0;
    }
}

void GroomShape::stopCommitLanding() {
    if (gLandCallback != 0) MMessage::removeCallback(gLandCallback);
    gLandCallback = 0;
    gPendingCommits.clear();
}

void GroomShape::settleCommit(const char* who) {
    // A stroke chained on the commit: the session is as the stroke found it (its document, the guides
    // committed), which is what the stroke reads; its commit goes on from this one.
    if (chain_ != Chain::None) return;
    finishCommit(false, nullptr, who);
}

bool GroomShape::chainStroke() {
    if (!commitPending_ || pendingKind_ != CommitKind::Guides || chain_ != Chain::None || !asyncCommitsEnabled() ||
        !gpuLookPreviewFor(this)) {
        return false;
    }
    chain_ = Chain::Provisional;
    return true;
}

void GroomShape::finishCommit(bool evaluatesAfter, MDataBlock* into, const char* who) {
    if (!commitPending_) return;
    commitPending_ = false;
    chain_ = Chain::None;  // (taken in under a chained stroke: the stroke goes on from it, on the CPU)
    pendingKind_ = CommitKind::None;
    settledBy_ = who ? who : "";
    gPendingCommits.erase(this);
    // Every detached evaluation run (in order); the last one's document becomes the session's. One
    // that failed, or a session on another graph since, adopts nothing: the evaluation below (or
    // compute's) makes the results itself, and says what failed.
    std::shared_ptr<DetachedEvaluation> run = commit_.wait();
    lastCommitMs_ = run ? run->milliseconds() : 0.0;
    ++commits_;
    if (run) session_.adopt(*run);
    run.reset();  // (the session's document before it goes)
    // Compute's evaluate goes on: it evaluates (the commit's work reused: nothing changed since, or
    // only what changed then) and makes the frame.
    if (evaluatesAfter) {
        pendingErrors_.clear();
        return;
    }
    // Its frame: the session's evaluation, which finds every result made.
    const ViewResult& result = session_.evaluate(pendingRequest_);
    const std::string joined = finishView(result, pendingRequest_, std::move(pendingErrors_));
    pendingErrors_.clear();
    // Its frame becomes the outputs', unless an input dirtied them since (the next pull evaluates,
    // from the session as the commit left it).
    auto land = [&](MDataBlock& data) {
        if (!data.isClean(aOutFrame)) return;
        ++live_.evaluations;
        makeFrame(data, live_, &result, pendingSeconds_, joined);
    };
    try {
        if (into) {
            land(*into);
        } else {
            MDataBlock data = forceCache();
            land(data);
        }
    } catch (const std::exception& e) {
        report(std::string("evaluation failed: ") + e.what(), true);
    }
    // The stroke document warmed at once, off the UI thread (the next press finds it evaluated, or
    // waits for what is left of it), not on the next idle.
    if (this == gBrushTarget && requested_ && !hasPreview() && !strokeWarmupOff()) {
        if (gWarmCallback != 0) MMessage::removeCallback(gWarmCallback);
        gWarmCallback = 0;
        try {
            startWarm();
        } catch (...) {
        }
    } else {
        scheduleStrokeWarmup();
    }
    MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
    if (!computing_) M3dView::scheduleRefreshAllViews();
}

void GroomShape::makeFrame(MDataBlock& data, Lane& lane, const ViewResult* result, double seconds, const std::string& errors) {
    auto frame = std::make_shared<GroomFrame>();
    const GroomFrame* last = lane.frame.get();
    if (result) {
        frame->view = *result;
        frame->view.messages.clear();
        // What changed, for the viewport: the same cached results (a static groom at another
        // frame) keep the versions, and the GPU buffers, as they were.
        const bool strandsChanged = !last || result->versions.strands != lane.seen.strands || result->strands != last->view.strands;
        const bool hierarchyChanged = !last || result->versions.hierarchy != lane.seen.hierarchy;
        const bool cardsChanged = !last || result->versions.cards != lane.seen.cards;
        const bool strandMeshChanged = !last || result->versions.strandMeshes != lane.seen.strandMeshes;
        const bool anyChanged = !last || result->versions.evaluation != lane.seen.evaluation || strandsChanged;
        frame->strandsVersion = strandsChanged ? nextVersion() : last->strandsVersion;
        frame->hierarchyVersion = hierarchyChanged ? nextVersion() : last->hierarchyVersion;
        frame->cardsVersion = cardsChanged ? nextVersion() : last->cardsVersion;
        frame->strandMeshVersion = strandMeshChanged ? nextVersion() : last->strandMeshVersion;
        frame->version = anyChanged ? nextVersion() : last->version;
        if (strandsChanged || hierarchyChanged) {
            const Bounds& b = result->bounds;
            frame->bounds = b.valid ? MBoundingBox(MPoint(b.lo.x, b.lo.y, b.lo.z), MPoint(b.hi.x, b.hi.y, b.hi.z)) : MBoundingBox();
        } else {
            frame->bounds = last->bounds;
        }
        lane.seen = result->versions;
        // renderView: the render's width and length scales on the strands drawn (render_overrides.h),
        // as strands of their own (no serial: the display builds them whole). Strands scaled by s
        // about their roots stay within the bounds grown about their center by 2s - 1.
        RenderOverrides overrides;
        overrides.widthScale = lane.renderWidthScale;
        overrides.lengthScale = lane.renderLengthScale;
        if (lane.renderView && result->strands && !overrides.keepsShape()) {
            frame->view.strands = applyRenderOverrides(result->strands, overrides);
            frame->view.strandsSerial = frame->view.changesBase = 0;
            frame->view.strandsChanges.reset();
            frame->strandsVersion = frame->version = nextVersion();
            const double grow = std::max(1.0, 2.0 * overrides.lengthScale - 1.0);
            if (grow > 1.0 && result->bounds.valid) {
                const MPoint c = frame->bounds.center();
                const MVector half = 0.5 * grow * (frame->bounds.max() - frame->bounds.min());
                frame->bounds = MBoundingBox(c - half, c + half);
            }
        }
    } else {
        // A failed evaluation: no strands (the rest of the last frame stays), every version moved.
        if (last) frame->view = last->view;
        frame->view.strands.reset();
        frame->view.strandsSerial = frame->view.changesBase = 0;
        frame->view.strandsChanges.reset();
        frame->bounds = last ? last->bounds : MBoundingBox();
        frame->version = frame->strandsVersion = nextVersion();
        frame->hierarchyVersion = last ? last->hierarchyVersion : nextVersion();
        frame->cardsVersion = last ? last->cardsVersion : nextVersion();
        frame->strandMeshVersion = last ? last->strandMeshVersion : nextVersion();
    }
    frame->seconds = seconds;
    frame->errors = errors;
    frame->liveEvaluation = lane.live ? lane.evaluations : 0;
    lane.frame = frame;
    writeFrame(data, frame);
}

MStatus GroomShape::setDependentsDirty(const MPlug& plug, MPlugArray& affected) {
    // What names the groom's files: their groomLabFile nodes follow on idle (file_node.h).
    if (plug == aScalpMaps || plug == aCardMaps || plug == aGuideCache || plug == aGroomFile || plug == aSimCaches || plug == aGraph) {
        MString call("try:\n    from groomlab import dependencies\n    dependencies.sync_soon('");
        call += MFnDependencyNode(thisMObject()).name();
        call += "')\nexcept ImportError:\n    pass";
        MGlobal::executePythonCommandOnIdle(call);
    }
    // Display-only: the viewport redraws what it has (the draw override reads them on a version change).
    const bool display =
        plug == aGuideSelection || plug == aSoftSelectRadius || plug == aStrandColorMode ||
        plug == aStrandColorSource || plug == aStrandColorMin || plug == aStrandColorMax || plug == aDisplayRibbons ||
        plug == aRibbonWidthScale || plug == aDisplayWidthCoverage || plug == aDisplayCoverageDensity || plug == aDisplayClumpLod || plug == aClumpLodError || plug == aSculptDisplay || plug == aIsolateGroup || plug == aGuideColorMode ||
        plug == aGuideColorLayer || plug == aDisplayInstances || plug == aDisplayLighting || plug == aDisplayShadows || plug == aDisplayHairShading || plug == aDisplaySelfShadowing || plug == aDisplayStrandVariation ||
        plug == aGuideWidth || plug == aGuidePoints || plug == aGuideSmoothing || plug == aHiddenGuideGroups || plug == aGuideGroupColors ||
        plug == aDisplayFlow || plug == aDisplayColor ||
        plug == aUseMaterialColor || plug == aDisplayGuides || plug == aDisplayHair || plug == aDisplayTubes || plug == aDisplayCards || plug == aDisplayMesh ||
        plug == aDisplayCardTextures || plug == aDisplayOverlaysOnTop || plug == aCardMaps || plug == aScalpMapDisplay || plug == aScalpMapChannel;
    // Inputs: the groom re-evaluates, then redraws.
    const bool input =
        plug == aInMesh || plug == aRestMesh || plug == aFollowScalp || plug == aScalpTopology || plug == aScaleSource || plug == aScaleReference || plug == aScaleTransformReference || plug == aColliderMeshes ||
        plug == aColliderMesh ||
        plug == aColliderName || plug == aColliderRestMesh || plug == aExtraScalps || plug == aExtraScalpMesh ||
        plug == aExtraScalpName || plug == aExtraScalpRestMesh || plug == aSimGuides || plug == aSimCurve || plug == aGuideCache ||
        plug == aGuideCacheObject || plug == aSimCaches || plug == aGuideCurves || plug == aTime || plug == aGraph || plug == aGuideData ||
        plug == aPartData || plug == aFlowData || plug == aCorrectiveData || plug == aCorrectiveWeight || plug == aViewNode ||
        plug == aSculptData || plug == aSculptNode ||
        plug == aSculptDensity || plug == aPreviewDensity || plug == aPreviewPointBudget || plug == aRenderView || plug == aCardDensity ||
        plug == aBrushTick || plug == aScalpMaps;
    // Control attributes (dynamic, added per Control node): the groom re-evaluates.
    const bool control = !display && !input && isControlAttribute(plug.attribute());
    if (control) {
        for (const MObject* out : {&aOutStrandCount, &aLastError, &aOutFrame, &aOutCards, &aOutRachis, &aOutMesh}) affected.append(MPlug(thisMObject(), *out));
    }
    // The expensive conversions and the stored texts are read again only for inputs that were
    // dirtied (or whose data reads differently; see computeStrands).
    if (input) markDirty(plug);
    if (plug == aBrushTick || plug == aScalpMaps) ++mapInputsVersion_;
    if (display) ++displayVersion_;
    if (display || input || control) MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
    return MPxSurfaceShape::setDependentsDirty(plug, affected);
}

void GroomShape::watchScalpMesh(const MObject& mesh) {
    if (scalpAttrCallback_ != 0) MMessage::removeCallback(scalpAttrCallback_);
    scalpAttrCallback_ = 0;
    if (mesh.isNull() || !mesh.hasFn(MFn::kMesh)) return;
    MObject node = mesh;
    MStatus status;
    scalpAttrCallback_ = MNodeMessage::addAttributeChangedCallback(
        node,
        [](MNodeMessage::AttributeMessage msg, MPlug& plug, MPlug&, void* data) {
            if (!(msg & MNodeMessage::kAttributeSet) || MFileIO::isReadingFile()) return;
            const MString name = plug.partialName(false, false, false, false, false, true);
            if (name != "aiSubdivType" && name != "aiSubdivIterations" && name != "displaySmoothMesh" && name != "smoothLevel") {
                return;
            }
            // The scalp is read again (its render subdivision with it) and the groom evaluates:
            // brushTick is the input the tools bump for that.
            auto* self = static_cast<GroomShape*>(data);
            self->markDirty(MPlug(self->thisMObject(), aInMesh));
            MPlug tick(self->thisMObject(), aBrushTick);
            tick.setValue(tick.asInt() + 1);
        },
        this, &status);
    if (!status) scalpAttrCallback_ = 0;
}

MStatus GroomShape::connectionMade(const MPlug& plug, const MPlug& otherPlug, bool asSrc) {
    if (plug == aInMesh || plug == aRestMesh) live_.meshReconnected = true;  // maybe another mesh: converted in full
    if (plug == aInMesh && !asSrc) watchScalpMesh(otherPlug.node());
    // A card mesh, or a shading group (the lit strand color follows the material): redraw.
    if (plug == aOutCards || plug == instObjGroups) {
        ++displayVersion_;
        MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
    }
    markDirty(plug);
    return MPxSurfaceShape::connectionMade(plug, otherPlug, asSrc);
}

MStatus GroomShape::connectionBroken(const MPlug& plug, const MPlug& otherPlug, bool asSrc) {
    if (plug == aInMesh || plug == aRestMesh) live_.meshReconnected = true;  // maybe another mesh: converted in full
    if (plug == aInMesh && !asSrc) watchScalpMesh(MObject::kNullObj);
    if (plug == aOutCards || plug == instObjGroups) {
        ++displayVersion_;
        MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
    }
    markDirty(plug);
    return MPxSurfaceShape::connectionBroken(plug, otherPlug, asSrc);
}

std::map<GuideId, float> GroomShape::guideSelectionWeights() {
    const HierarchyPtr h = evaluatedHierarchy();
    const std::vector<GuideId> selected = readGuideSelection(thisMObject());
    if (!h || selected.empty()) return {};
    return softSelection(*h, selected, readSoftSelectRadius(thisMObject()));
}

std::uint64_t GroomShape::selectionHash() const {
    std::uint64_t hash = hash64(0x5E1);
    for (const GuideId id : readGuideSelection(thisMObject())) hash = hashCombine(hash, id);
    const float radius = readSoftSelectRadius(thisMObject());
    std::uint32_t bits = 0;
    std::memcpy(&bits, &radius, sizeof bits);
    return hashCombine(hash, bits);
}

FramePtr GroomShape::frame() {
    MStatus status;
    const MObject data = MPlug(thisMObject(), aOutFrame).asMObject(&status);  // evaluates live when dirty
    if (!status || data.isNull()) return live_.frame;
    MFnPluginData fn(data, &status);
    const auto* frameData = status ? dynamic_cast<const GroomFrameData*>(fn.constData()) : nullptr;
    return frameData && frameData->frame ? frameData->frame : live_.frame;
}

void GroomShape::ensureEvaluated() {
    FramePtr shown = frame();
    // A commit evaluating off the UI thread (one may just have started in that pull): the session
    // is the worker's until it lands; landed, the frame is its.
    if (commitPending_) {
        settleCommit("ensureEvaluated");
        shown = frame();
    }
    if (!shown || shown == live_.frame) return;
    // The frame shown came from cached playback (or its background fill): the live session
    // evaluates it again, in the normal context, so what tools read is what is drawn. Its frame
    // replaces the cached one in the data block (the same groom at the same time).
    MDataBlock data = forceCache();
    try {
        computeStrands(data);
    } catch (const std::exception& e) {
        report(std::string("evaluation failed: ") + e.what(), true);
    }
}

StrandsPtr GroomShape::evaluatedStrands() {
    const FramePtr f = frame();
    return f ? f->view.placed() : nullptr;
}

StrandsPtr GroomShape::handedStrands() {
    const FramePtr f = frame();
    return f ? f->view.strands : nullptr;
}

FollowCarryPtr GroomShape::evaluatedFollow() {
    const FramePtr f = frame();
    return f ? f->view.follow : nullptr;
}

MStatus GroomShape::computeCards(MDataBlock& data, Lane& lane) {
    data.inputValue(aOutStrandCount);  // the evaluation that makes the Cards nodes' cards
    // Connected since that evaluation, which skipped the cards as unwanted: build them now.
    const std::shared_ptr<const NodeLinks> taken = lane.live ? nullptr : links();
    const bool connected = taken ? taken->cardsConnected : MPlug(thisMObject(), aOutCards).isConnected();
    // A commit evaluating off the UI thread (it builds no cards: none were wanted) and still no card
    // mesh connected: the frame's cards, the session left to the worker. Connected since: waited for.
    std::shared_ptr<const CardMesh> cards;
    if (lane.live && commitPending_ && connected) finishCommit(false, &data, "outCards");
    if (lane.live && commitPending_) {
        cards = lane.frame ? lane.frame->view.cards : nullptr;
    } else {
        if (!lane.session.cardsEvaluated() && connected) evaluate(data, lane);
        cards = lane.session.last().cards;  // the same cards as last time (another input dirtied the plug): the same mesh
    }
    if (!lane.cardsMeshMade || cards != lane.cardsMeshFrom) {
        MFnMeshData meshData;
        MObject meshObject = meshData.create();
        if (cards && !cards->quads.empty()) {
            MFloatPointArray points;
            points.setLength(static_cast<unsigned>(cards->positions.size()));
            for (unsigned i = 0; i < points.length(); ++i) {
                const Vec3f& p = cards->positions[i];
                points[i] = MFloatPoint(p.x, p.y, p.z);
            }
            MIntArray counts(static_cast<unsigned>(cards->quads.size() / 4), 4);
            MIntArray connects;
            connects.setLength(static_cast<unsigned>(cards->quads.size()));
            for (unsigned i = 0; i < connects.length(); ++i) connects[i] = static_cast<int>(cards->quads[i]);
            MFloatArray us, vs;
            us.setLength(static_cast<unsigned>(cards->vertexCount()));
            vs.setLength(us.length());
            for (unsigned i = 0; i < us.length(); ++i) {
                us[i] = cards->u[i];
                vs[i] = cards->v[i];
            }
            MFnMesh mesh;
            MStatus status;
            mesh.create(static_cast<int>(points.length()), static_cast<int>(counts.length()), points, counts, connects, us,
                        vs, meshObject, &status);
            if (status) {
                mesh.assignUVs(counts, connects);  // UV ids follow the vertex ids
                // What the cards shade with in the groom's own drawing, for whatever takes this
                // mesh (Export for Unreal's FBX): the strands' lit normals (locked), and each
                // card's tint as the color set groomLabTint (half the tint: 0.5 none).
                if (cards->normals.size() == cards->positions.size()) {
                    MVectorArray normals(points.length());
                    MIntArray ids(points.length());
                    for (unsigned i = 0; i < points.length(); ++i) {
                        const Vec3f& n = cards->normals[i];
                        normals[i] = MVector(n.x, n.y, n.z);
                        ids[i] = static_cast<int>(i);
                    }
                    mesh.setVertexNormals(normals, ids);
                }
                if (cards->tints.size() == cards->positions.size()) {
                    MString tintSet("groomLabTint");
                    tintSet = mesh.createColorSetDataMesh(tintSet);
                    MColorArray tints(points.length());
                    for (unsigned i = 0; i < points.length(); ++i) {
                        const auto& t = cards->tints[i];
                        tints[i] = MColor(0.5f * t[0], 0.5f * t[1], 0.5f * t[2], 1.0f);
                    }
                    if (mesh.setColors(tints, &tintSet) == MS::kSuccess) mesh.assignColors(connects, &tintSet);
                }
            }
        }
        lane.cardsMeshData = meshObject;
        lane.cardsMeshFrom = cards;
        lane.cardsMeshMade = true;
    }
    data.outputValue(aOutCards).set(lane.cardsMeshData);
    data.setClean(aOutCards);
    return MS::kSuccess;
}

MStatus GroomShape::computeRachis(MDataBlock& data, Lane& lane) {
    // Not connected: nothing reads the mesh, so the full-density evaluation is not made (the
    // Evaluation Manager computes every output it dirtied, connected or not, on every frame).
    const std::shared_ptr<const NodeLinks> taken = lane.live ? nullptr : links();
    if (!(taken ? taken->rachisConnected : MPlug(thisMObject(), aOutRachis).isConnected())) {
        if (lane.unusedRachisData.isNull()) lane.unusedRachisData = MFnMeshData().create();
        data.outputValue(aOutRachis).set(lane.unusedRachisData);
        data.setClean(aOutRachis);
        return MS::kSuccess;
    }
    data.inputValue(aOutStrandCount);  // the evaluation that hands the session its inputs (scalp, guides...)
    if (lane.live) finishCommit(false, &data, "outRachis");  // (the session is the worker's while a commit evaluates)
    StrandsPtr fullStrands;
    try {
        // The groom at full density, on the session's own copy of the graph (the viewport's cache
        // is left alone): the Propagate nodes' shafts, every feather, as the exports see them.
        fullStrands = lane.session.evaluateFullDensity(mayaSeconds(data.inputValue(aTime).asTime()));
    } catch (const std::exception& e) {
        if (lane.live) report(std::string("rachis: ") + e.what(), true);
    }
    // The same strands as last time (a cache hit: another input dirtied the plug): the same mesh.
    if (lane.rachisMeshMade && fullStrands == lane.rachisMeshFrom) {
        data.outputValue(aOutRachis).set(lane.rachisMeshData);
        data.setClean(aOutRachis);
        return MS::kSuccess;
    }
    MFnMeshData meshData;
    MObject meshObject = meshData.create();
    RachisMesh rachis;
    try {
        if (fullStrands) rachis = buildRachisMesh(fullStrands->featherLayers());
    } catch (const std::exception& e) {
        if (lane.live) report(std::string("rachis: ") + e.what(), true);
        rachis = RachisMesh{};
    }
    auto ints = [](const std::vector<int>& from) {
        MIntArray out;
        out.setLength(static_cast<unsigned>(from.size()));
        for (unsigned i = 0; i < out.length(); ++i) out[i] = from[i];
        return out;
    };
    auto floats = [](const auto& from) {
        MFloatArray out;
        out.setLength(static_cast<unsigned>(from.size()));
        for (unsigned i = 0; i < out.length(); ++i) out[i] = static_cast<float>(from[i]);
        return out;
    };
    if (!rachis.faceCounts.empty()) {
        MFloatPointArray points;
        points.setLength(static_cast<unsigned>(rachis.points.size()));
        for (unsigned i = 0; i < points.length(); ++i) {
            const Vec3f& p = rachis.points[i];
            points[i] = MFloatPoint(p.x, p.y, p.z);
        }
        const MIntArray counts = ints(rachis.faceCounts), connects = ints(rachis.faceVertices), uvIds = ints(rachis.faceUVs);
        const MFloatArray us = floats(rachis.u), vs = floats(rachis.v);
        MFnMesh mesh;
        MStatus status;
        mesh.create(static_cast<int>(points.length()), static_cast<int>(counts.length()), points, counts, connects, us, vs,
                    meshObject, &status);
        if (status) {
            mesh.assignUVs(counts, uvIds);
            // Which feather each vertex belongs to, and its group: a UV set of their own (it goes
            // through FBX to the engine), one value per vertex.
            MString featherSet("featherId");
            mesh.createUVSetDataMesh(featherSet);
            MFloatArray fu, fv;
            fu.setLength(static_cast<unsigned>(rachis.points.size()));
            fv.setLength(fu.length());
            for (unsigned i = 0; i < fu.length(); ++i) {
                fu[i] = static_cast<float>(rachis.featherIndex[i]);
                fv[i] = static_cast<float>(rachis.groupIds[i]);
            }
            mesh.setUVs(fu, fv, &featherSet);
            mesh.assignUVs(counts, connects, &featherSet);
        }
    }
    lane.rachisMeshData = meshObject;
    lane.rachisMeshFrom = fullStrands;
    lane.rachisMeshMade = true;
    data.outputValue(aOutRachis).set(meshObject);
    data.setClean(aOutRachis);
    return MS::kSuccess;
}

namespace {

// The file name base of a bake into `directory`: `base` made file safe, with _2, _3... when
// another groom's card bake has that prefix there (its viewport reads those files).
std::string freeBakeBase(const MObject& groom, const std::string& directory, const std::string& base) {
    const std::string safe = fileSafeName(base);
    std::string name = safe;
    for (int n = 2; bakePrefixUsedByOtherGroom(groom, directory + "/" + name + "_"); ++n) name = safe + "_" + std::to_string(n);
    return name;
}

}  // namespace

CardBake GroomShape::bakeCardsInMemory(int resolution, float occlusion, const std::string& graphJson, NodeEvaluator* evaluator) {
    // The cards as the viewport and export make them: the groom at cardDensity with its Cards
    // nodes building cards, laid out in one atlas (every host's bake). No card mesh needed.
    std::string error;
    StrandsPtr strands;
    MeshPtr scalp;
    const float density = MPlug(thisMObject(), aCardDensity).asFloat();
    if (evaluator) {
        evaluator->setGraphJson(graphJson);
        strands = evaluator->evaluate(density, &error, &scalp, true);
    } else {
        strands = evaluateGroomNode(thisMObject(), density, &error, &scalp, true, graphJson.empty() ? nullptr : &graphJson);
    }
    if (!strands) throw std::runtime_error(error.empty() ? "the groom has no strands" : error);
    return groomlab::bakeCardsInMemory(std::move(strands), std::move(scalp), resolution, occlusion);
}

std::vector<std::string> GroomShape::exportCardMaps(const std::string& directory, const std::string& base, const CardMapPreset& preset,
                                                     const CardMapExport& settings, float occlusion) {
    std::string error;
    MeshPtr scalp;
    const float density = MPlug(thisMObject(), aCardDensity).asFloat();
    StrandsPtr strands = evaluateGroomNode(thisMObject(), density, &error, &scalp, true, nullptr);
    if (!strands) throw std::runtime_error(error.empty() ? "the groom has no strands" : error);
    const CardBake bake = groomlab::bakeCardMapsInMemory(std::move(strands), std::move(scalp), settings, occlusion);
    return writeCardMaps(*bake.maps, preset, settings, directory, fileSafeName(base));
}

std::vector<std::string> GroomShape::bakeCards(const std::string& directory, const std::string& requestedBase, int resolution,
                                                float occlusion, bool store, const std::string& layout) {
    if (layout != "unreal" && layout != "unity") throw std::runtime_error("the layout is unreal or unity, not " + layout);
    const CardBake bake = bakeCardsInMemory(resolution, occlusion);
    const CardTextures& t = bake.textures;
    const std::string base = freeBakeBase(thisMObject(), directory, requestedBase);
    std::vector<std::string> paths;
    if (layout == "unity") {  // an export: PNGs laid out for Unity's materials, never shown by the groom
        const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
        const auto smoothness = static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - hairLook().roughness)));
        std::vector<std::uint8_t> baseColor(4 * n), mask(4 * n);
        for (std::size_t i = 0; i < n; ++i) {
            baseColor[4 * i] = t.color[3 * i];
            baseColor[4 * i + 1] = t.color[3 * i + 1];
            baseColor[4 * i + 2] = t.color[3 * i + 2];
            baseColor[4 * i + 3] = t.coverage[i];
            mask[4 * i] = t.iridescence.size() == n ? t.iridescence[i] : 0;  // metallic: none; the iridescence, if any
            mask[4 * i + 1] = t.occlusion[i];          // ambient occlusion
            mask[4 * i + 2] = t.attribute[4 * i + 3];  // root (0) to tip (1), in the detail mask's slot
            mask[4 * i + 3] = smoothness;              // 1 - the hair look's roughness
        }
        const std::vector<std::uint8_t> normal = cardNormalOpenGl(t);  // Unity's green: OpenGL's
        // Encoded side by side (writeCompressedPngs: the bytes of one at a time).
        std::vector<PngImage> images;
        auto png = [&](const char* map, int channels, const std::vector<std::uint8_t>& pixels) {
            paths.push_back(directory + "/" + base + "_" + map + ".png");
            images.push_back({t.size, t.size, channels, &pixels});
        };
        png("BaseColor", 4, baseColor);
        png("Mask", 4, mask);
        png("AO", 1, t.occlusion);
        png("Tangent", 3, t.tangent);
        png("Depth", 1, t.depth);
        png("Attribute", 4, t.attribute);
        png("Normal", 3, normal);
        writeCompressedPngs(paths, images);
        return paths;
    }
    // Unreal's hair card maps and the atlas, as every host writes them (card_files.h); the app's
    // preview is made when the groom is exported for it.
    const CardBakePaths at = CardBakePaths::in(directory, base);
    const std::string bakeStamp = writeCardBake(bake, at, false);
    paths = at.files();
    if (!store) return paths;  // an export: the groom keeps showing its own bake
    // The cards view shows them from now on.
    static int bakes = 0;  // a new stamp each bake: the viewport reloads maps rewritten in place
    // Stored portable (relative to the scene's folder or the project when inside one), so the
    // scene and its groomlab/ folder move together.
    MPlug(thisMObject(), aCardMaps).setString(formatCardMaps(portableMayaPath(at.prefix), bakeStamp, ++bakes).c_str());
    return paths;
}

ScalpTextures GroomShape::bakeScalpInMemory(int resolution, float reach, float density, MeshPtr* scalpOut,
                                            NodeEvaluator* evaluator) {
    std::string error;
    MeshPtr scalp;
    StrandsPtr strands;
    if (evaluator) {
        evaluator->setGraphJson({});  // the groom's own graph
        strands = evaluator->evaluate(std::clamp(density, 0.0f, 1.0f), &error, &scalp);
    } else {
        strands = evaluateGroomNode(thisMObject(), std::clamp(density, 0.0f, 1.0f), &error, &scalp);
    }
    if (!strands) throw std::runtime_error(error.empty() ? "the groom has no strands" : error);
    if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the groom has no scalp");
    ScalpBakeSettings settings;
    settings.size = resolution;
    settings.reach = reach;
    ScalpTextures t;
    try {
        t = bakeScalpTextures(*strands, *scalp, settings);
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(std::string(e.what()) + ": give the scalp mesh UVs");
    }
    if (scalpOut) *scalpOut = scalp;
    return t;
}

std::vector<std::string> GroomShape::bakeScalp(const std::string& directory, const std::string& requestedBase, int resolution,
                                               float reach, float density, const std::string& layout) {
    if (layout != "unreal" && layout != "unity") throw std::runtime_error("the layout is unreal or unity, not " + layout);
    ScalpTextures t = bakeScalpInMemory(resolution, reach, density, nullptr);
    const std::string base = freeBakeBase(thisMObject(), directory, requestedBase);
    if (layout == "unity") {
        const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
        std::vector<std::uint8_t> baseColor(4 * n);
        for (std::size_t i = 0; i < n; ++i) {
            baseColor[4 * i] = t.color[3 * i];
            baseColor[4 * i + 1] = t.color[3 * i + 1];
            baseColor[4 * i + 2] = t.color[3 * i + 2];
            baseColor[4 * i + 3] = t.coverage[i];
        }
        const std::string path = directory + "/" + base + "_ScalpBaseColor.png";
        writeCompressedPng(path, t.size, t.size, 4, baseColor);
        return {path};
    }
    std::vector<std::string> paths{directory + "/" + base + "_ScalpColor.tga", directory + "/" + base + "_ScalpCoverage.tga"};
    parallelInvoke([&] { writeTga(paths[0], t.size, t.size, 3, t.color); },
                   [&] { writeTga(paths[1], t.size, t.size, 1, t.coverage); });
    return paths;
}

std::string GroomShape::graphStatsJson() {
    settleCommit("graphStats");
    // The last live evaluation's (no evaluation when the frame shown is a cached one: the HUD and
    // the Graph Editor ask during playback), with the drawn frame's counts.
    const FramePtr shown = frame();
    GroomStats stats = session_.stats();
    if (shown && shown != live_.frame) {
        stats.strands = shown->view.strands ? shown->view.strands->strandCount() : 0;
        stats.points = shown->view.strands ? shown->view.strands->pointCount() : 0;
    }
    // The scalp's earlier frames Jiggle nodes read: how many, and how many times inMesh was
    // evaluated at another time so far (the cost of the history: 0 more per frame while playing).
    stats.scalpHistoryFrames = scalpHistoryFrames_;
    stats.scalpHistoryReads = scalpPast_.reads();
    return stats.toJson();
}

SimRun GroomShape::simulateTo(int frame, const SimProgress& progress) {
    ensureEvaluated();  // the session holds this frame's inputs
    prepareSimulation(thisMObject(), session_, live_.simInputs);
    const SimRun run = session_.simulateTo(frame, progress);
    // The frames the view reads may be new (a node in cacheMode "cached"): evaluated again.
    MPlug tick(thisMObject(), aBrushTick);
    tick.setInt(tick.asInt() + 1);
    return run;
}

void GroomShape::invalidateSimulation(const std::string& node) {
    settleCommit("invalidateSimulation");
    session_.invalidateSimulation(node);
    MPlug tick(thisMObject(), aBrushTick);
    tick.setInt(tick.asInt() + 1);
}

void GroomShape::setLiveDrag(bool on) {
    if (session_.liveDrag() == on) return;
    settleCommit("setLiveDrag");
    session_.setLiveDrag(on);
    MPlug tick(thisMObject(), aBrushTick);  // (the input the tools bump to evaluate the groom again)
    tick.setInt(tick.asInt() + 1);
}

std::vector<SimStatus> GroomShape::simulationStatus() {
    ensureEvaluated();
    return session_.simulationStatus();
}

Value GroomShape::evaluateNode(const std::string& name, float density) {
    ensureEvaluated();
    // A scratch copy of the graph: results that match the viewport's are reused, and what this
    // evaluation makes never evicts them.
    return session_.evaluateNode(name, density, mayaSeconds(MPlug(thisMObject(), aTime).asMTime()));
}

GroomShape::HairLook GroomShape::hairLook() {
    frame();  // the graph as evaluated (the look is the graph's, at any frame)
    settleCommit("hairLook");
    return session_.hairLook();
}

std::string GroomShape::hairLookJson() {
    frame();
    settleCommit("hairLook");
    return session_.hairLookJson();
}

HierarchyPtr GroomShape::evaluatedHierarchy() {
    const FramePtr f = frame();
    return f ? f->view.hierarchy : nullptr;
}

HierarchyPtr GroomShape::authoredHierarchy() {
    ensureEvaluated();
    // A stroke chained on a pending commit: the guides it committed (the session bound them when it
    // detached the commit), not those the session's last evaluation had.
    if (chain_ != Chain::None) return session_.authoredHierarchy();
    return session_.last().authoredHierarchy;
}

bool GroomShape::cardMeshConnected() const { return MPlug(thisMObject(), aOutCards).isConnected(); }

bool GroomShape::cardsView() {
    if (!MPlug(thisMObject(), aDisplayCards).asBool()) return false;
    const std::shared_ptr<const CardMesh> cards = evaluatedCards();
    return (cards && !cards->cards.empty()) || cardMeshConnected();
}

std::vector<StrandMeshSource> GroomShape::evaluatedStrandMeshes() {
    const FramePtr f = frame();
    return f ? f->view.strandMeshes : std::vector<StrandMeshSource>{};
}

std::uint64_t GroomShape::strandMeshVersion() {
    const FramePtr f = frame();
    return f ? f->strandMeshVersion : 0;
}

bool GroomShape::strandMeshConnected() const { return MPlug(thisMObject(), aOutMesh).isConnected(); }

std::shared_ptr<const CardMesh> GroomShape::evaluatedCards() {
    const FramePtr f = frame();
    return f ? f->view.cards : nullptr;
}

FlowPtr GroomShape::evaluatedFlow() {
    const FramePtr f = frame();
    return f ? f->view.flow : nullptr;
}

PartsPtr GroomShape::evaluatedParts() {
    const FramePtr f = frame();
    return f ? f->view.parts : nullptr;
}

MeshPtr GroomShape::evaluatedScalp() {
    const FramePtr f = frame();
    return f ? f->view.scalp : nullptr;
}

MeshPtr GroomShape::evaluatedCageScalp() {
    const FramePtr f = frame();
    return f ? f->view.cageScalp : nullptr;
}

bool GroomShape::showsRoots() {
    const FramePtr f = frame();
    return f && f->view.viewedRoots;
}

std::shared_ptr<const std::vector<groomlab::Vec3f>> GroomShape::clumpCenters() {
    const FramePtr f = frame();
    return f ? f->view.clumpCenters : nullptr;
}

std::string GroomShape::viewedNode() {
    const FramePtr f = frame();
    return f ? f->view.viewedNode : std::string();
}

float GroomShape::evaluatedDensity() {
    const FramePtr f = frame();
    return f ? f->view.density : 1.0f;
}

std::uint64_t GroomShape::evaluationVersion() {
    const FramePtr f = frame();
    return f ? f->version : 0;
}

std::uint64_t GroomShape::strandsVersion() {
    const FramePtr f = frame();
    return f ? f->strandsVersion : 0;
}

std::uint64_t GroomShape::strandsSerial() {
    const FramePtr f = frame();
    return f ? f->view.strandsSerial : 0;
}

std::uint64_t GroomShape::strandsChangesBase() {
    const FramePtr f = frame();
    return f ? f->view.changesBase : 0;
}

ChangesPtr GroomShape::strandsChanges() {
    const FramePtr f = frame();
    return f ? f->view.strandsChanges : nullptr;
}

std::uint64_t GroomShape::hierarchyVersion() {
    const FramePtr f = frame();
    return f ? f->hierarchyVersion : 0;
}

std::uint64_t GroomShape::cardsVersion() {
    const FramePtr f = frame();
    return f ? f->cardsVersion : 0;
}

std::uint64_t GroomShape::backgroundEvaluationCount() {
    std::lock_guard<std::recursive_mutex> lock(background_->mutex);
    return background_->lane.evaluations;
}

HierarchyPtr GroomShape::posedHierarchy() {
    ensureEvaluated();
    return session_.posedHierarchy();
}

const ScalpMaps& GroomShape::scalpMaps(std::string* error) {
    settleCommit("scalpMaps");
    session_.setScalpMapsJson(MPlug(thisMObject(), aScalpMaps).asString().asChar());
    if (error) *error = session_.scalpMapsError();
    return session_.scalpMaps();
}

void GroomShape::setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) {
    if (values) unchain("setPreviewVertexMap");
    session_.setPreviewVertexMap(name, std::move(values), channels);
    MPlug tick(thisMObject(), aBrushTick);
    tick.setInt(tick.asInt() + 1);
}

void GroomShape::setPreviewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) {
    if (image) unchain("setPreviewTextureMap");
    session_.setPreviewTextureMap(name, std::move(image));
    ++displayVersion_;
    MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
}

GroomShape::ScalpMapView GroomShape::scalpMapView() {
    settleCommit("scalpMapView");  // (the viewport asks only while none is pending)
    const FramePtr f = frame();
    const MeshPtr scalp = f ? f->view.scalp : nullptr;
    std::uint64_t key = hashCombine(hash64(0x3A9F1E), f ? f->version : 0);
    key = hashCombine(key, f ? f->hierarchyVersion : 0);
    key = hashCombine(key, scalp ? scalp->contentHash() : 0);
    key = hashCombine(key, displayVersion_);  // scalpMapDisplay, scalpMapChannel (and the brush's image)
    key = hashCombine(key, session_.previewMapVersion());
    key = hashCombine(key, mapInputsVersion_);  // scalpMaps, brushTick
    key = hashCombine(key, fileWrites());
    const auto now = std::chrono::steady_clock::now();
    MapViewMemo& memo = mapViewMemo_;
    if (memo.valid && memo.key == key &&
        (memo.view.kind != "texture" || now - memo.stampChecked < std::chrono::milliseconds(500))) {
        return memo.view;
    }
    memo.view = makeScalpMapView();
    memo.key = key;
    memo.valid = true;
    memo.stampChecked = now;
    return memo.view;
}

GroomShape::ScalpMapView GroomShape::makeScalpMapView() {
    scalpMaps();  // the maps as stored now
    session_.setPathResolver(nodePathResolver(thisMObject()));  // a texture map's file, as the node resolves it
    return session_.scalpMapView(MPlug(thisMObject(), aScalpMapDisplay).asString().asChar(),
                                 MPlug(thisMObject(), aScalpMapChannel).asString().asChar());
}

void GroomShape::setPreviewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) {
    // A press: the stroke document warmed off the UI thread taken in first (waited for when still
    // evaluating: it got that far already).
    if (preview && !session_.hasPreview()) takeWarm(true);
    MPlug tick(thisMObject(), aBrushTick);
    if (preview && !final && chain_ == Chain::Provisional) {
        // A stroke pressed while a stroke's commit evaluates: the GPU preview arms now, on the
        // session's document as it was, so even the press's evaluation stops at its node; refused,
        // the commit is taken in first, as for any other stroke.
        session_.setPreviewHierarchy(preview, final);
        GpuLookPreview* gpu = gpuPreviewEnabled() ? gpuLookPreviewFor(this) : nullptr;
        if (gpu && gpu->armStroke(*this)) {
            chain_ = Chain::Armed;
            ++chainedStrokes_;
            tick.setInt(tick.asInt() + 1);
            return;
        }
        if (gpu) gpu->endStroke(*this);  // (it may arm once the commit is in, as after any press)
        session_.setPreviewHierarchy(nullptr);
    }
    if (preview && chain_ != Chain::Armed) {
        unchain("setPreviewHierarchy");
    } else if (!preview) {
        chain_ = Chain::None;  // the stroke is over (its commit chains on the pending one: compute)
    }
    session_.setPreviewHierarchy(std::move(preview), final);
    tick.setInt(tick.asInt() + 1);
}

void GroomShape::unchain(const char* who) {
    chain_ = Chain::None;
    settleCommit(who);
}

void GroomShape::setLiveStroke(bool on) {
    if (on) takeWarm(true);  // (the stroke document it evaluates)
    if (on) unchain("setLiveStroke");
    if (on == session_.liveStroke()) return;
    ++displayVersion_;  // the strands step back, or come back (sculptDisplay)
    if (session_.setLiveStroke(on)) {
        // Something evaluated at the stroke's density: evaluate again at the full one.
        MPlug tick(thisMObject(), aBrushTick);
        tick.setInt(tick.asInt() + 1);
    }
    MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());
}

void GroomShape::setPreviewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) {
    // (None set, none before: nothing to draw again, nothing to evaluate. A stroke's press says so
    // for the guides alone, before it sets their preview: a stroke chained on a pending commit
    // evaluates nothing until the GPU took it.)
    if (!parts && !flow && !session_.hasPreviewOverlays()) return;
    if (parts || flow) unchain("setPreviewOverlays");
    session_.setPreviewOverlays(std::move(parts), std::move(flow));
    MPlug tick(thisMObject(), aBrushTick);
    tick.setInt(tick.asInt() + 1);
}

void GroomShape::setPreviewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown) {
    if (!edits && !session_.hasPreviewSculpt()) return;
    if (edits) unchain("setPreviewSculpt");
    session_.setPreviewSculpt(node, std::move(edits), std::move(shown));
    MPlug tick(thisMObject(), aBrushTick);
    tick.setInt(tick.asInt() + 1);
}

SculptInput GroomShape::sculptInput(const std::string& node) {
    settleCommit("sculptInput");
    return session_.sculptInput(node);
}

MBoundingBox GroomShape::boundingBox() const {
    const FramePtr f = const_cast<GroomShape*>(this)->frame();
    const MBoundingBox bounds = f ? f->bounds : MBoundingBox();
    if (bounds.width() == 0.0 && bounds.height() == 0.0 && bounds.depth() == 0.0) {
        return MBoundingBox(MPoint(-0.5, -0.5, -0.5), MPoint(0.5, 0.5, 0.5));
    }
    return bounds;
}

std::string guideColorSeedOf(const MObject& groomShape) {
    MStatus status;
    MFnDagNode dag(groomShape, &status);
    if (!status) return {};
    const MObject parent = dag.parent(0, &status);
    return (status && !parent.isNull() ? MFnDependencyNode(parent).name() : dag.name()).asChar();
}

void* GroomShapeUI::creator() { return new GroomShapeUI(); }

}  // namespace groomlab::maya
