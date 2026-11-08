// SPDX-License-Identifier: Apache-2.0
// MtoA extension: renders groomLabShape in Arnold.
//
// renderMode "curves" (default): the translator evaluates the groom's graph
// itself at full density (the viewport's previewDensity does not apply) and
// exports a curves node. With deformation motion blur on, the groom is
// evaluated at every motion step (OR-5); steps need the same strands, which
// a rest mesh guarantees on a deforming scalp. Instance nodes' geometry goes
// to an instancer whose instance_matrix gets a key per motion step too (and
// the groom's transform keys as its matrix); scales go to one polymesh whose
// points and normals get a key per motion step, bent onto the skin of that step.
//
// renderMode "groomFile": exports a groomlab_procedural node that renders the
// groom's groomFile at render time (OR-3), so .ass files stay small and
// render on a farm without Maya.
//
// Render LOD (lodDistance, cameraCull, backfaceCull and their settings on the
// groom): the strands are thinned with distance from the render camera and
// culled outside its view or rooted facing away from it; every motion step
// keeps the same strands. The camera is
// the one Arnold renders through, read from its Maya camera (complete at any
// point of the export; the procedural reads the Arnold camera at render time,
// when it is complete too: lodCameraFromArnold).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ai.h>
#include <maya/MCallbackIdArray.h>
#include <maya/MDGMessage.h>
#include <maya/MDagPath.h>
#include <maya/MEventMessage.h>
#include <maya/MSceneMessage.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnCamera.h>
#include <maya/MItDag.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnMesh.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MObjectArray.h>
#include <maya/MMatrix.h>
#include <maya/MSelectionList.h>
#include <maya/MPlug.h>
#include <maya/MTime.h>
#include <maya/MUuid.h>

#include "attributes/AttrHelper.h"
#include "extension/Extension.h"
#include "session/SessionOptions.h"
#include "translators/shape/ShapeTranslator.h"

#include "groomlab/arnold_curves.h"
#include "groomlab/asset_files.h"
#include "groomlab/export_view.h"
#include "groomlab/hair_color.h"
#include "groomlab/lod.h"
#include "groomlab/render_overrides.h"
#include "groomlab/scales.h"
#include "maya_inputs.h"

namespace {

const AtString kShader("shader");
const AtString kMelanin("melanin");
const char* const kDefaultShaderTag = "defaultHairShader";

// ---------------------------------------------------------------------------
// Evaluators kept from one export to the next, one per groom (its node's UUID and the instance
// rendered). MtoA makes a translator for every frame of a sequence, and an interactive render
// exports the groom again after each edit: the kept evaluator's graph cache, parsed inputs and
// converted Maya data carry over (NodeEvaluator::refreshStoredInputs reads the edits), so a
// static groom's next frame is a cache hit and a moving one re-runs only what moves. The output
// is what a fresh evaluator gives.
//
// Memory (each holds its groom's evaluation at render density, intermediate results too: about
// three times the strands it renders): an entry goes when its node is deleted, on a new or
// opened scene, when the extension unloads, on the first idle after no translator holds it (the
// end of an interactive render or export; a sequence export or a batch render has no idle
// between its frames), and, of those no translator holds, all but the kKeptUnused used last
// (a script exporting many grooms one after another, with no idle between).
constexpr std::size_t kKeptUnused = 4;

struct Kept {
    std::shared_ptr<groomlab::maya::NodeEvaluator> evaluator;
    std::uint64_t lastUsed = 0;
};
std::mutex gKeptMutex;
std::map<std::string, Kept> gKept;
std::uint64_t gKeptClock = 0;
MCallbackIdArray gKeptCallbacks;
MCallbackId gIdleCallback = 0;
bool gIdleWanted = false;

std::string keptKey(const MObject& node, unsigned instance) {
    return std::string(MFnDependencyNode(node).uuid().asString().asChar()) + "#" + std::to_string(instance);
}

std::shared_ptr<groomlab::maya::NodeEvaluator> keptEvaluator(const MObject& node, unsigned instance) {
    std::lock_guard<std::mutex> lock(gKeptMutex);
    Kept& kept = gKept[keptKey(node, instance)];
    if (!kept.evaluator) kept.evaluator = std::make_shared<groomlab::maya::NodeEvaluator>(node, true);
    kept.lastUsed = ++gKeptClock;
    return kept.evaluator;
}

void dropKept() {
    std::lock_guard<std::mutex> lock(gKeptMutex);
    gKept.clear();
}

// Drops the entries no translator holds past the `keep` used last (gKeptMutex held).
void dropUnusedLocked(std::size_t keep) {
    std::vector<std::pair<std::uint64_t, std::string>> unused;
    for (const auto& [key, kept] : gKept) {
        if (kept.evaluator.use_count() == 1) unused.emplace_back(kept.lastUsed, key);
    }
    if (unused.size() <= keep) return;
    std::sort(unused.begin(), unused.end());  // least recently used first
    for (std::size_t i = 0; i + keep < unused.size(); ++i) gKept.erase(unused[i].second);
}

// Idle: the entries no translator holds go (the render or export that used them is over).
void dropUnusedOnIdle(void*) {
    {
        std::lock_guard<std::mutex> lock(gKeptMutex);
        dropUnusedLocked(0);
        gIdleWanted = false;
    }
    if (gIdleCallback != 0) {
        MMessage::removeCallback(gIdleCallback);
        gIdleCallback = 0;
    }
}

// A translator let go of its evaluator: the unused entries past kKeptUnused go now, the rest on
// the next idle.
void releaseKept(std::shared_ptr<groomlab::maya::NodeEvaluator>& evaluator) {
    {
        std::lock_guard<std::mutex> lock(gKeptMutex);
        evaluator.reset();
        dropUnusedLocked(kKeptUnused);
        if (gIdleWanted) return;
        gIdleWanted = true;
    }
    MStatus status;
    gIdleCallback = MEventMessage::addEventCallback("idle", dropUnusedOnIdle, nullptr, &status);
    if (!status) {
        gIdleCallback = 0;
        std::lock_guard<std::mutex> lock(gKeptMutex);
        gIdleWanted = false;
    }
}

void dropKeptOfNode(MObject& node, void*) {
    const std::string prefix = std::string(MFnDependencyNode(node).uuid().asString().asChar()) + "#";
    std::lock_guard<std::mutex> lock(gKeptMutex);
    for (auto it = gKept.lower_bound(prefix); it != gKept.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
        it = gKept.erase(it);
    }
}

void dropKeptOnScene(void*) { dropKept(); }

void registerKeptCallbacks() {
    MStatus status;
    MCallbackId id = MDGMessage::addNodeRemovedCallback(dropKeptOfNode, "groomLabShape", nullptr, &status);
    if (status) gKeptCallbacks.append(id);
    for (const MSceneMessage::Message message : {MSceneMessage::kBeforeNew, MSceneMessage::kBeforeOpen}) {
        id = MSceneMessage::addCallback(message, dropKeptOnScene, nullptr, &status);
        if (status) gKeptCallbacks.append(id);
    }
}

void removeKeptCallbacks() {
    if (gKeptCallbacks.length() > 0) MMessage::removeCallbacks(gKeptCallbacks);
    gKeptCallbacks.clear();
    if (gIdleCallback != 0) MMessage::removeCallback(gIdleCallback);
    gIdleCallback = 0;
    gIdleWanted = false;
    dropKept();
}

class GroomTranslator : public CShapeTranslator {
public:
    static void* creator() { return new GroomTranslator(); }

    ~GroomTranslator() override {
        if (m_evaluator) releaseKept(m_evaluator);
    }

    static void NodeInitializer(CAbTranslator context) {
        CExtensionAttrHelper helper(context.maya, "curves");
        MakeCommonAttributes(helper);
        helper.MakeInput("mode");
        helper.MakeInput("min_pixel_width");
    }

    AtNode* CreateArnoldNodes() override {
        m_procedural = mayaPlug("renderMode").asShort() == 1;
        return AddArnoldNode(m_procedural ? "groomlab_procedural" : "curves");
    }

    void Export(AtNode* node) override {
        pauseRender();
        groomlab::maya::refreshMayaPaths();  // this DLL keeps its own copy
        ExportMatrix(node);
        ProcessRenderFlags(node);
        // aiMode lists Arnold's curves modes; "oriented" needs orientations, which
        // Arnold does not support on linear curves, so it renders as ribbon.
        const int mode = mayaPlug("aiMode").asShort();
        if (!groomlab::arnold::curvesModeKnown(mode)) {
            AiMsgWarning("[groomlab] %s: aiMode 'oriented' is not available for linear curves; rendering ribbons",
                         GetMayaNodeName().asChar());
        }
        AiNodeSetStr(node, AtString("mode"), AtString(groomlab::arnold::curvesMode(mode)));
        ProcessParameter(node, "min_pixel_width", AI_TYPE_FLOAT);
        try {  // never let an exception reach MtoA (a render-density bad_alloc, say)
            if (m_procedural) {
                ExportProcedural(node);
            } else {
                ExportStrands(node);
            }
        } catch (const std::exception& e) {
            AiMsgError("[groomlab] %s: %s", GetMayaNodeName().asChar(), e.what());
            m_reference.reset();  // no motion keys for strands that never exported
        }
        setMotionRange(node);
        if (RequiresShaderExport()) ExportShaders();
    }

    void ExportMotion(AtNode* node) override {
        pauseRender();
        CShapeTranslator::ExportMotion(node);  // transform keys
        if (m_procedural || !m_reference) return;  // the procedural evaluates its own keys

        try {
            std::string error;
            groomlab::MeshPtr scalp;
            const groomlab::StrandsPtr strands = m_evaluator ? groomlab::applyRenderOverrides(
                m_evaluator->evaluate(m_overrides.densityTimes(1.0f), &error, &scalp), m_overrides) : nullptr;
            if (m_scales) ExportScalesKey(strands, scalp);
            if (m_rachis) ExportRachisKey(strands);
            // The instances at this step: their own key of instance_matrix, and the groom's transform keys.
            if (m_instancer) {
                if (const AtArray* matrix = AiNodeGetArray(node, AtString("matrix"))) {
                    AiNodeSetArray(m_instancer, AtString("matrix"), AiArrayCopy(matrix));
                }
                if (m_instanceKeys > 1 &&
                    !(strands && groomlab::arnold::setInstancerKey(m_instancer, m_instanceLayout, *strands, GetMotionStep())) &&
                    !m_instancesWarned) {
                    m_instancesWarned = true;
                    AiMsgWarning("[groomlab] %s: instances change between motion steps (give the groom a rest mesh); "
                                 "they render without deformation blur on those steps",
                                 GetMayaNodeName().asChar());
                }
            }
            if (strands && groomlab::arnold::sameTopology(*m_full, *strands)) {
                // The curves' plan (the LOD's, less the strands Arnold cannot take) picks the kept
                // strands straight into this key: no copy of the set.
                groomlab::arnold::setPointsKey(node, *strands, GetMotionStep(), curvesPlan());
                return;
            }
        } catch (const std::exception& e) {
            AiMsgError("[groomlab] %s: %s", GetMayaNodeName().asChar(), e.what());
            return;
        }
        if (!m_warned) {
            m_warned = true;
            AiMsgWarning("[groomlab] %s: strands change between motion steps (give the groom a rest mesh); "
                         "rendering without deformation blur",
                         GetMayaNodeName().asChar());
        }
    }

    void ExportShaders() override {
        AtNode* shader = nullptr;
        MPlug shadingGroup = GetNodeShadingGroup(m_dagPath.node(), m_dagPath.instanceNumber());
        if (!shadingGroup.isNull()) shader = ExportConnectedNode(shadingGroup);
        if (!shader) shader = ExportDefaultHairShader();
        // `shader` is a NODE[] on curves and on the procedural (kick -info): one element.
        groomlab::arnold::setShader(GetArnoldNode(), shader);
    }

    // A groom with no shader of its own renders by its look, as the viewport shows it (the core's
    // GroomSession::hairLook, which create_hair_shader builds from too): a standard_hair reading
    // the RGB (the hair_color user data, over no melanin) when the look colors by RGB (HairColor's
    // mode color, a map, a tip color, hue randomness, Bands, FeatherColor, a Blend of two colors,
    // a colorSource), else the pigment (the melanin and melaninRedness user data) under HairColor's
    // dye; its highlight HairColor's (hairLobes). (It rendered the default brown before, whatever
    // the groom's colors.)
    AtNode* ExportDefaultHairShader() {
        groomlab::HairLook look;
        if (!m_evaluator || !m_evaluator->hairLook(&look)) {
            try {  // the procedural: the look is the graph's alone
                groomlab::GroomData data;
                data.graphJson = mayaPlug("graph").asString().asChar();
                look = groomlab::GroomSession(data).hairLook();
            } catch (const std::exception&) {
                look = groomlab::HairLook{};
            }
        }
        auto node = [&](const char* type, const char* tag) {
            AtNode* made = GetArnoldNode(tag);
            return made ? made : AddArnoldNode(type, tag);
        };
        AtNode* shader = node("standard_hair", kDefaultShaderTag);
        const groomlab::HairLobes lobes = groomlab::hairLobes(look);
        AiNodeSetRGB(shader, AtString("specular_tint"), lobes.specularTint, lobes.specularTint, lobes.specularTint);
        AiNodeSetFlt(shader, AtString("roughness"), lobes.roughness);
        AiNodeSetFlt(shader, AtString("shift"), lobes.shiftDegrees);
        AiNodeSetFlt(shader, AtString("melanin_randomize"), 0.0f);  // HairColor varies it
        for (const char* input : {"base_color", "melanin", "melanin_redness"}) AiNodeUnlink(shader, input);
        if (look.colorsByRgb || look.colorBlends) {
            AtNode* color = node("user_data_rgb", "defaultHairShaderColor");
            AiNodeSetStr(color, AtString("attribute"), AtString("hair_color"));
            const std::array<float, 3> fallback = groomlab::defaultHairColor();
            AiNodeSetRGB(color, AtString("default"), fallback[0], fallback[1], fallback[2]);
            // Arnold absorbs its base color more than the viewport's mapping: the color to the power
            // 1 / kArnoldBaseColorGamma (hair_color.h) renders as the viewport shades it.
            AtNode* gamma = node("color_correct", "defaultHairShaderGamma");
            AiNodeSetFlt(gamma, AtString("gamma"), groomlab::kArnoldBaseColorGamma);
            AiNodeLink(color, "input", gamma);
            AiNodeLink(gamma, "base_color", shader);
            AiNodeSetFlt(shader, kMelanin, 0.0f);
        } else {
            AiNodeSetRGB(shader, AtString("base_color"), look.dye[0], look.dye[1], look.dye[2]);
            const struct {
                const char* input;
                const char* channel;
                const char* tag;
                float fallback;
            } pigment[] = {{"melanin", groomlab::kMelaninChannel, "defaultHairShaderMelanin", groomlab::kDefaultMelanin},
                           {"melanin_redness", groomlab::kMelaninRednessChannel, "defaultHairShaderRedness",
                            groomlab::kDefaultMelaninRedness}};
            for (const auto& p : pigment) {
                AtNode* data = node("user_data_float", p.tag);
                AiNodeSetStr(data, AtString("attribute"), AtString(p.channel));
                AiNodeSetFlt(data, AtString("default"), p.fallback);
                AiNodeLink(data, p.input, shader);
            }
        }
        return shader;
    }

private:
    MPlug mayaPlug(const char* name) { return MFnDependencyNode(GetMayaObject()).findPlug(name, true); }

    // The groom's render overrides (render_overrides.h): renders only, the viewport as groomed.
    groomlab::RenderOverrides renderOverrides() {
        groomlab::RenderOverrides o;
        o.density = mayaPlug("renderDensity").asFloat();
        o.widthScale = mayaPlug("renderWidthScale").asFloat();
        o.lengthScale = mayaPlug("renderLengthScale").asFloat();
        return o;
    }

    unsigned motionKeys() { return RequiresMotionData() ? GetNumMotionSteps() : 1u; }

    // Motion keys span these frames, relative to the exported frame.
    void setMotionRange(AtNode* node) {
        unsigned count = 0;
        const double* frames = RequiresMotionData() ? GetMotionFrames(count) : nullptr;
        const double frame = GetExportFrame();
        const float start = frames && count > 1 ? static_cast<float>(frames[0] - frame) : 0.0f;
        const float end = frames && count > 1 ? static_cast<float>(frames[count - 1] - frame) : 0.0f;
        AiNodeSetFlt(node, AtString("motion_start"), start);
        AiNodeSetFlt(node, AtString("motion_end"), end);
    }

    void ExportStrands(AtNode* curves) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string error;
        groomlab::MeshPtr scalp;
        // One evaluator for this frame and every motion step, kept for the next frame and the
        // next interactive update (keptEvaluator): only what moves, or was edited, re-evaluates.
        if (!m_evaluator) m_evaluator = keptEvaluator(GetMayaObject(), m_dagPath.instanceNumber());
        m_evaluator->refreshStoredInputs();  // edits since its last export
        m_overrides = renderOverrides();     // every motion step renders with this frame's
        m_reference = groomlab::applyRenderOverrides(m_evaluator->evaluate(m_overrides.densityTimes(1.0f), &error, &scalp),
                                                     m_overrides);
        if (!error.empty()) AiMsgWarning("[groomlab] %s: %s", GetMayaNodeName().asChar(), error.c_str());
        if (!m_reference) m_reference = std::make_shared<groomlab::StrandSet>();
        m_warned = false;
        m_instancer = nullptr;
        m_instanceKeys = 1;
        m_instancesWarned = false;
        m_scales = nullptr;
        m_scaleMesh.reset();
        m_scaleBuilder.clear();
        m_scenePrototypes.clear();
        m_scalesWarned = false;
        m_rachis = nullptr;
        m_rachisMesh = groomlab::RachisMesh{};
        m_rachisWarned = false;
        m_full = m_reference;
        m_lod = lodPlan(curves, *m_full, scalp.get());
        if (m_lod) {
            m_reference = std::make_shared<groomlab::StrandSet>(groomlab::applyLod(*m_full, *m_lod));
            AiMsgInfo("[groomlab] %s: LOD keeps %zu of %zu strands (%zu outside the view, %zu facing away, %zu thinned)",
                      GetMayaNodeName().asChar(), m_lod->kept.size(), m_full->strandCount(), m_lod->culled,
                      m_lod->backfacing, m_lod->thinned);
        }

        // The curves take the strands Arnold can render (curvesPlan: none with fewer than two
        // points or a point that is not finite); the instances and scales lay out from them all.
        std::size_t unrenderable = 0;
        m_curvesPlan = groomlab::arnold::curvesPlan(*m_full, m_lod ? &*m_lod : nullptr, &unrenderable);
        if (unrenderable > 0) {
            AiMsgWarning("[groomlab] %s: %zu strands with fewer than two points or a point that is not a number are "
                         "left out of the curves",
                         GetMayaNodeName().asChar(), unrenderable);
        }
        const groomlab::StrandsPtr curveStrands =
            m_curvesPlan ? std::make_shared<groomlab::StrandSet>(groomlab::applyLod(*m_full, *m_curvesPlan)) : m_reference;

        // Every key starts as this frame; ExportMotion fills the others. Every array is made in
        // full from this one evaluation before it is set (the counts all agree).
        const groomlab::ExportView view = groomlab::describeExport(*curveStrands, scalp.get(), groomlab::kExportPointColors);
        groomlab::arnold::setCurves(curves, view, motionKeys());
        groomlab::arnold::setStrandData(curves, view, scalp.get(), &m_declared);
        // Strands that only lay scales out render as the scales alone.
        if (m_reference->instances() && m_reference->instances()->hidesStrands) AiNodeSetByte(curves, AtString("visibility"), 0);
        ExportInstances(curves, scalp);
        ExportRachis(curves);
        warnHeavyInteractive(curveStrands->strandCount(), t0);
    }

    // The feathers' rachis (a Propagate node's shaftWidth; feathers.h): their tubes as one polymesh
    // (setRachisMesh) rendered with a standard_surface reading each feather's shaft color; the
    // shafts stay out of the curves (curvesPlan). Not when a mesh of the scene shows it (a live
    // rachis mesh on outRachis, create_rachis_mesh): MtoA renders that one.
    void ExportRachis(AtNode* curves) {
        if (!groomlab::hasRachis(m_full->featherLayers())) return;
        MPlug out = mayaPlug("outRachis");
        if (!out.isNull() && out.isSource()) return;
        groomlab::RachisMesh mesh = groomlab::buildRachisMesh(m_full->featherLayers());
        if (mesh.faceCount() == 0) return;
        AtNode* surface = AddArnoldNode("standard_surface", "rachisSurface");
        groomlab::arnold::setDefaultRachisShader(surface, AddArnoldNode("user_data_rgb", "rachisColor"));
        AtNode* polymesh = AddArnoldNode("polymesh", "rachis");
        // As many point keys as the curves; ExportMotion fills them (ExportRachisKey).
        groomlab::arnold::setRachisMesh(polymesh, mesh, groomlab::rachisColors(*m_full, mesh, {0.5f, 0.45f, 0.4f}), motionKeys());
        groomlab::arnold::setShader(polymesh, surface);
        if (const AtArray* matrix = AiNodeGetArray(curves, AtString("matrix"))) {
            AiNodeSetArray(polymesh, AtString("matrix"), AiArrayCopy(matrix));
        }
        setMotionRange(polymesh);
        if (motionKeys() > 1) {
            m_rachis = polymesh;
            m_rachisMesh = std::move(mesh);
        }
    }

    // This motion step's key of the rachis: from the step's feathers.
    void ExportRachisKey(const groomlab::StrandsPtr& strands) {
        if (strands && groomlab::arnold::setRachisMeshKey(m_rachis, m_rachisMesh, groomlab::buildRachisMesh(strands->featherLayers()),
                                                          GetMotionStep())) {
            return;
        }
        if (!m_rachisWarned) {
            m_rachisWarned = true;
            AiMsgWarning("[groomlab] %s: the feathers' rachis changes between motion steps; it renders without "
                         "deformation blur on those steps",
                         GetMayaNodeName().asChar());
        }
    }

    // A groom this heavy in an interactive render: every edit exports all of it again (the
    // evaluation, then Arnold's rebuild of the curves), so the Script Editor says so, once per
    // render, with what it costs and how to make it lighter.
    void warnHeavyInteractive(std::size_t strands, std::chrono::steady_clock::time_point t0) {
        constexpr std::size_t kHeavyInteractiveStrands = 500000;
        if (m_heavyWarned || strands < kHeavyInteractiveStrands || !IsInteractiveSession() || IsFileExport() || IsBatchSession()) return;
        m_heavyWarned = true;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char message[512];
        std::snprintf(message, sizeof message,
                      "GroomLab: %s renders %.1fM strands: each interactive update exports them all again (%.1f s "
                      "here) and Arnold rebuilds them. Lower its renderDensity while editing the look.",
                      GetMayaNodeName().asChar(), static_cast<double>(strands) / 1e6, seconds);
        AiMsgWarning("[groomlab] %s", message);
        MGlobal::displayWarning(message);
    }

    // MtoA pauses an interactive render before it updates a translator; should a version not,
    // the render is interrupted here before any array is replaced (interruptRenderForEdit),
    // said once in the log.
    void pauseRender() {
        if (!groomlab::arnold::interruptRenderForEdit(GetUniverse()) || m_pauseWarned) return;
        m_pauseWarned = true;
        AiMsgWarning("[groomlab] %s: Arnold was still rendering when the groom was updated; paused it first",
                     GetMayaNodeName().asChar());
    }

    // The strands the curves take, over m_full: the LOD's plan, or the curves' own when it leaves
    // out more (curvesPlan); null for every strand.
    const groomlab::LodPlan* curvesPlan() const {
        if (m_curvesPlan) return &*m_curvesPlan;
        return m_lod ? &*m_lod : nullptr;
    }

    // The Instance node's geometry: each prototype mesh as a hidden polymesh
    // (with its own shading group), placed by an instancer; scales as one
    // polymesh of their own (ExportScales).
    void ExportInstances(AtNode* curves, const groomlab::MeshPtr& scalp) {
        const auto& instances = m_reference->instances();
        if (!instances || instances->empty()) return;
        if (instances->hasMeshes()) ExportScales(curves, scalp);
        if (std::all_of(instances->instances.begin(), instances->instances.end(),
                        [](const groomlab::InstanceSet::Instance& i) { return i.meshed(); })) {
            return;
        }
        std::vector<AtNode*> prototypes;
        for (std::size_t p = 0; p < instances->prototypes.size(); ++p) {
            prototypes.push_back(ExportPrototype(instances->prototypes[p], static_cast<int>(p)));
        }
        AtNode* instancer = AddArnoldNode("instancer", "instances");
        if (const AtArray* matrix = AiNodeGetArray(curves, AtString("matrix"))) {
            AiNodeSetArray(instancer, AtString("matrix"), AiArrayCopy(matrix));
        }
        // As many instance_matrix keys as the curves have point keys; ExportMotion fills them.
        if (!groomlab::arnold::setInstancer(instancer, prototypes, *m_reference, motionKeys(), &m_instanceLayout)) {
            AiMsgWarning("[groomlab] %s: nothing to instance (are the prototype meshes in the scene?)",
                         GetMayaNodeName().asChar());
            return;
        }
        m_instancer = instancer;
        m_instanceKeys = motionKeys();
        setMotionRange(instancer);
    }

    // The scales (scales.h): bent onto the scalp, one polymesh carrying each scale's data as user
    // data (scaleColor, scaleRootColor, scaleId, scaleUV, scaleVariant). Each takes its Instance
    // node's material, else its prototype mesh's shading group, else (the built-in plates) a
    // standard_surface reading scaleColor.
    void ExportScales(AtNode* curves, const groomlab::MeshPtr& scalp) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto& instances = m_reference->instances();
        const auto mesh = m_scaleBuilder.build(*m_reference, scalp, scaleLookup());
        if (!mesh || mesh->triangles.empty()) return;
        std::vector<AtNode*> shaders;
        std::map<std::pair<std::int32_t, std::uint32_t>, std::uint8_t> slotOf;  // (material, prototype)
        std::vector<std::uint8_t> slots(mesh->scales.size(), 0);
        AtNode* fallback = nullptr;
        for (std::size_t k = 0; k < mesh->scales.size(); ++k) {
            const groomlab::ScaleMesh::Scale& s = mesh->scales[k];
            const auto key = std::make_pair(s.material, s.prototype);
            auto it = slotOf.find(key);
            if (it == slotOf.end()) {
                AtNode* shader = nullptr;
                if (s.material >= 0 && static_cast<std::size_t>(s.material) < instances->materials.size()) {
                    shader = ExportMaterial(instances->materials[static_cast<std::size_t>(s.material)]);
                }
                MDagPath path;
                const std::string& prototype = s.prototype < instances->prototypes.size() ? instances->prototypes[s.prototype] : "";
                if (!shader && !groomlab::builtinPrototype(prototype)) {
                    if (!groomlab::maya::prototypePath(prototype, &path)) {
                        AiMsgWarning("[groomlab] %s: no mesh '%s' for the scales' shading", GetMayaNodeName().asChar(), prototype.c_str());
                    } else if (!(shader = ExportMeshShader(path))) {
                        AiMsgWarning("[groomlab] %s: '%s' has no shading group: the default scale surface",
                                     GetMayaNodeName().asChar(), path.fullPathName().asChar());
                    }
                }
                if (!shader) {
                    if (!fallback) {
                        fallback = AddArnoldNode("standard_surface", "scaleSurface");
                        groomlab::arnold::setDefaultScaleShader(fallback, AddArnoldNode("user_data_rgb", "scaleColor"));
                    }
                    shader = fallback;
                }
                const auto found = std::find(shaders.begin(), shaders.end(), shader);
                const std::size_t slot = found != shaders.end() ? static_cast<std::size_t>(found - shaders.begin()) : shaders.size();
                if (found == shaders.end() && shaders.size() < 255) shaders.push_back(shader);
                it = slotOf.emplace(key, static_cast<std::uint8_t>(std::min<std::size_t>(slot, shaders.size() - 1))).first;
            }
            slots[k] = it->second;
        }
        AtNode* polymesh = AddArnoldNode("polymesh", "scales");
        // As many point keys as the curves; ExportMotion fills them (ExportScalesKey).
        groomlab::arnold::setScaleMesh(polymesh, *mesh, shaders, slots, motionKeys());
        if (const AtArray* matrix = AiNodeGetArray(curves, AtString("matrix"))) {
            AiNodeSetArray(polymesh, AtString("matrix"), AiArrayCopy(matrix));
        }
        setMotionRange(polymesh);
        if (motionKeys() > 1) {
            m_scales = polymesh;
            m_scaleMesh = mesh;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        AiMsgInfo("[groomlab] %s: %zu scales, %zu triangles (%.0f ms)", GetMayaNodeName().asChar(), mesh->scales.size(),
                  mesh->triangleCount(), ms);
    }

    // The scene's prototype meshes by name, read once per export.
    groomlab::PrototypeLookup scaleLookup() {
        return [this](const std::string& name) -> const groomlab::InstancePrototype* {
            auto it = m_scenePrototypes.find(name);
            if (it == m_scenePrototypes.end()) {
                it = m_scenePrototypes.emplace(name, groomlab::InstancePrototype{}).first;
                groomlab::maya::readInstancePrototype(name, it->second);
            }
            return it->second.found ? &it->second : nullptr;
        };
    }

    // This motion step's key of the scales: the strands of the step (as rendered: through the
    // LOD plan) bent onto its skin; only the scales that moved are bent again.
    void ExportScalesKey(const groomlab::StrandsPtr& strands, const groomlab::MeshPtr& scalp) {
        std::shared_ptr<const groomlab::ScaleMesh> mesh;
        if (strands && groomlab::arnold::sameTopology(*m_full, *strands)) {
            const groomlab::StrandSet kept = m_lod ? groomlab::applyLod(*strands, *m_lod) : *strands;
            mesh = m_scaleBuilder.build(kept, scalp, scaleLookup());
        }
        if (mesh && groomlab::arnold::setScaleMeshKey(m_scales, *m_scaleMesh, *mesh, GetMotionStep())) return;
        if (!m_scalesWarned) {
            m_scalesWarned = true;
            AiMsgWarning("[groomlab] %s: scales change between motion steps (give the groom a rest mesh); "
                         "they render without deformation blur on those steps",
                         GetMayaNodeName().asChar());
        }
    }

    // A prototype mesh's shader: its shading group's (the one on its instance, else the first one
    // its faces are in, as MFnMesh lists them), exported as MtoA exports a shading group; null
    // when it has none.
    AtNode* ExportMeshShader(const MDagPath& path) {
        MPlug shadingGroup = GetNodeShadingGroup(path.node(), path.instanceNumber());
        AtNode* shader = shadingGroup.isNull() ? nullptr : ExportConnectedNode(shadingGroup);
        if (shader) return shader;
        MObjectArray engines;
        MIntArray faces;
        if (MFnMesh(path).getConnectedShaders(path.instanceNumber(), engines, faces) != MS::kSuccess) return nullptr;
        for (unsigned i = 0; i < engines.length() && !shader; ++i) {
            MPlug message = MFnDependencyNode(engines[i]).findPlug("message", true);
            if (!message.isNull()) shader = ExportConnectedNode(message);
        }
        return shader;
    }

    // A material by name: a shading group (its surface shader, as MtoA exports it) or a shader.
    AtNode* ExportMaterial(const std::string& name) {
        MSelectionList sel;
        MObject node;
        if (name.empty() || sel.add(name.c_str()) != MS::kSuccess || sel.getDependNode(0, node) != MS::kSuccess) {
            AiMsgWarning("[groomlab] %s: no material '%s' for the scales", GetMayaNodeName().asChar(), name.c_str());
            return nullptr;
        }
        MFnDependencyNode fn(node);
        MPlug plug = fn.findPlug(node.hasFn(MFn::kShadingEngine) ? "message" : "outColor", true);
        if (plug.isNull()) plug = fn.findPlug("message", true);
        return plug.isNull() ? nullptr : ExportConnectedNode(plug);
    }

    AtNode* ExportPrototype(const std::string& name, int index) {
        if (const groomlab::InstancePrototype* builtin = groomlab::builtinPrototype(name)) {
            AtNode* proto = AddArnoldNode("polymesh", ("prototype" + std::to_string(index)).c_str());
            groomlab::arnold::setPrototypeMesh(proto, *builtin);
            AiNodeSetByte(proto, AtString("visibility"), 0);  // only its instances render
            return proto;
        }
        MSelectionList sel;
        MDagPath path;
        if (sel.add(name.c_str()) != MS::kSuccess || sel.getDagPath(0, path) != MS::kSuccess ||
            (!path.hasFn(MFn::kMesh) && (path.extendToShape() != MS::kSuccess || !path.hasFn(MFn::kMesh)))) {
            AiMsgWarning("[groomlab] %s: no mesh '%s' to instance", GetMayaNodeName().asChar(), name.c_str());
            return nullptr;
        }
        MFnMesh mesh(path);
        MFloatPointArray points;
        mesh.getPoints(points, MSpace::kObject);  // the prototype's own space: instances place it
        MIntArray triangleCounts, triangleVertices;
        mesh.getTriangles(triangleCounts, triangleVertices);
        std::vector<AtVector> vlist(points.length());
        for (unsigned i = 0; i < points.length(); ++i) vlist[i] = AtVector(points[i].x, points[i].y, points[i].z);
        std::vector<uint32_t> vidxs(triangleVertices.length());
        for (unsigned i = 0; i < triangleVertices.length(); ++i) vidxs[i] = static_cast<uint32_t>(triangleVertices[i]);
        const std::vector<uint32_t> nsides(vidxs.size() / 3, 3u);

        AtNode* proto = AddArnoldNode("polymesh", ("prototype" + std::to_string(index)).c_str());
        AiNodeSetArray(proto, AtString("vlist"), AiArrayConvert(static_cast<uint32_t>(vlist.size()), 1, AI_TYPE_VECTOR, vlist.data()));
        AiNodeSetArray(proto, AtString("vidxs"), AiArrayConvert(static_cast<uint32_t>(vidxs.size()), 1, AI_TYPE_UINT, vidxs.data()));
        AiNodeSetArray(proto, AtString("nsides"), AiArrayConvert(static_cast<uint32_t>(nsides.size()), 1, AI_TYPE_UINT, nsides.data()));
        AiNodeSetBool(proto, AtString("smoothing"), true);
        AiNodeSetByte(proto, AtString("visibility"), 0);  // only its instances render
        if (AtNode* shader = ExportMeshShader(path)) groomlab::arnold::setShader(proto, shader);
        return proto;
    }

    groomlab::LodSettings lodSettings() {
        groomlab::LodSettings s;
        s.distance = mayaPlug("lodDistance").asBool();
        s.nearDistance = mayaPlug("lodNear").asFloat();
        s.farDistance = mayaPlug("lodFar").asFloat();
        s.minDensity = mayaPlug("lodMinDensity").asFloat();
        s.widthCompensation = mayaPlug("lodWidthCompensation").asFloat();
        s.frustum = mayaPlug("cameraCull").asBool();
        s.margin = mayaPlug("cullMargin").asFloat();
        s.backface = mayaPlug("backfaceCull").asBool();
        s.backfaceAngle = mayaPlug("backfaceAngle").asFloat();
        return s;
    }

    // The first renderable camera (perspective or orthographic).
    static MDagPath renderableCamera() {
        for (MItDag it(MItDag::kDepthFirst, MFn::kCamera); !it.isDone(); it.next()) {
            MDagPath path;
            it.getPath(path);
            MFnCamera fn(path);
            if (fn.findPlug("renderable", true).asBool()) return path;
        }
        return MDagPath();
    }

    // The LOD camera from a Maya camera, in the groom's space. Used when the
    // options hold no camera yet (arnoldExportAss sets none, and the camera's
    // Arnold node may not have its matrix until its own translator runs, which
    // can be after this one): the Maya camera is always complete.
    static bool lodCameraFromMaya(const MDagPath& path, const AtMatrix& groomToWorld, groomlab::LodCamera* out) {
        if (!path.isValid() || !path.hasFn(MFn::kCamera)) return false;
        MFnCamera fn(path);
        MMatrix groom;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) groom[r][c] = groomToWorld.data[r][c];
        }
        const MMatrix toGroom = path.inclusiveMatrix() * groom.inverse();  // row vectors: camera -> world -> groom
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) out->matrix[r][c] = static_cast<float>(toGroom[r][c]);
        }
        const double aspect = fn.aspectRatio() > 0.0 ? fn.aspectRatio() : 1.0;
        if (fn.isOrtho()) {
            out->orthographic = true;
            out->orthoHalfWidth = static_cast<float>(0.5 * fn.orthoWidth());
            out->orthoHalfHeight = static_cast<float>(0.5 * fn.orthoWidth() / aspect);
            return true;
        }
        out->orthographic = false;
        out->tanHalfFovX = static_cast<float>(std::tan(0.5 * fn.horizontalFieldOfView()));
        out->tanHalfFovY = static_cast<float>(std::tan(0.5 * fn.verticalFieldOfView()));
        return true;
    }

    // The Maya camera behind an Arnold camera node: MtoA names it after the camera's DAG path
    // ("/persp/perspShape"), or its short name when full paths are off.
    static MDagPath mayaCameraOf(const AtNode* camera) {
        if (!camera) return MDagPath();
        const std::string name = AiNodeGetName(camera);
        std::string path = name;
        std::replace(path.begin(), path.end(), '/', '|');
        for (const std::string& candidate : {path, name}) {
            MSelectionList sel;
            MDagPath dag;
            if (candidate.empty() || sel.add(candidate.c_str()) != MS::kSuccess || sel.getDagPath(0, dag) != MS::kSuccess) continue;
            if (dag.hasFn(MFn::kCamera)) return dag;
        }
        return MDagPath();
    }

    // The LOD camera, always read from Maya, where it is complete: the session's export camera
    // (an interactive render), else the Maya camera of the camera the options render through
    // (arnoldExportAss sets no export camera). The Arnold camera node itself is the last resort:
    // its translator may run after this one, leaving its matrix at the origin and its lens at the
    // default for now (every strand near, nothing thinned: what made LOD flaky in exports).
    bool lodCamera(AtNode* curves, groomlab::LodCamera* cam) {
        const AtMatrix groomToWorld = AiNodeGetMatrix(curves, AtString("matrix"));
        if (lodCameraFromMaya(GetSessionOptions().GetExportCamera(), groomToWorld, cam)) return true;
        AtNode* camera = AiUniverseGetCamera(GetUniverse());
        if (lodCameraFromMaya(mayaCameraOf(camera), groomToWorld, cam)) return true;
        if (camera && groomlab::arnold::lodCameraFromArnold(camera, AiUniverseGetOptions(GetUniverse()), groomToWorld, cam)) {
            return true;
        }
        return lodCameraFromMaya(renderableCamera(), groomToWorld, cam);
    }

    // The LOD plan for this frame's strands; none when LOD is off or there is no camera.
    std::optional<groomlab::LodPlan> lodPlan(AtNode* curves, const groomlab::StrandSet& strands, const groomlab::ScalpMesh* scalp) {
        const groomlab::LodSettings settings = lodSettings();
        if (!settings.distance && !settings.frustum && !settings.backface) return std::nullopt;
        groomlab::LodCamera cam;
        if (!lodCamera(curves, &cam)) {
            AiMsgWarning("[groomlab] %s: LOD needs a render camera; rendering every strand", GetMayaNodeName().asChar());
            return std::nullopt;
        }
        return groomlab::planLod(strands, cam, settings, scalp);
    }

    void ExportProcedural(AtNode* proc) {
        const std::string stored = mayaPlug("groomFile").asString().asChar();
        if (stored.empty()) {
            AiMsgWarning("[groomlab] %s: renderMode is groomFile but groomFile is empty", GetMayaNodeName().asChar());
        }
        // Paths with variables stay as they are, for the render machine to
        // expand; relative ones resolve against the project now.
        const bool hasVariables = stored.find_first_of("$%") != std::string::npos;
        const std::string file = hasVariables ? stored : groomlab::maya::resolveNodePath(GetMayaObject(), stored);
        AiNodeSetStr(proc, AtString("filename"), AtString(file.c_str()));
        // The groom's own entry of a file of several grooms (its asset name, as save_groom writes it).
        MDagPath groomPath;
        if (MDagPath::getAPathTo(GetMayaObject(), groomPath) == MS::kSuccess) {
            if (groomPath.node().hasFn(MFn::kShape)) groomPath.pop();
            AiNodeSetStr(proc, AtString("groom"), AtString(groomlab::fileSafeName(groomPath.fullPathName().asChar()).c_str()));
        }
        AiNodeSetFlt(proc, AtString("frame"), static_cast<float>(GetExportFrame()));
        AiNodeSetFlt(proc, AtString("fps"), static_cast<float>(MTime(1.0, MTime::kSeconds).as(MTime::uiUnit())));
        const groomlab::RenderOverrides overrides = renderOverrides();  // the procedural applies them
        AiNodeSetFlt(proc, AtString("density"), overrides.densityTimes(1.0f));
        AiNodeSetFlt(proc, AtString("width_scale"), overrides.widthScale);
        AiNodeSetFlt(proc, AtString("length_scale"), overrides.lengthScale);
        AiNodeSetInt(proc, AtString("motion_keys"), static_cast<int>(motionKeys()));
        const groomlab::LodSettings lod = lodSettings();
        AiNodeSetBool(proc, AtString("lod_distance"), lod.distance);
        AiNodeSetFlt(proc, AtString("lod_near"), lod.nearDistance);
        AiNodeSetFlt(proc, AtString("lod_far"), lod.farDistance);
        AiNodeSetFlt(proc, AtString("lod_min_density"), lod.minDensity);
        AiNodeSetFlt(proc, AtString("lod_width_compensation"), lod.widthCompensation);
        AiNodeSetBool(proc, AtString("camera_cull"), lod.frustum);
        AiNodeSetFlt(proc, AtString("cull_margin"), lod.margin);
        AiNodeSetBool(proc, AtString("backface_cull"), lod.backface);
        AiNodeSetFlt(proc, AtString("backface_angle"), lod.backfaceAngle);
    }

    bool m_procedural = false;
    bool m_warned = false;
    groomlab::RenderOverrides m_overrides;  // this frame's (renderOverrides), for its motion steps too
    groomlab::StrandsPtr m_reference;  // this frame's strands as rendered (after LOD)
    groomlab::StrandsPtr m_full;       // before LOD; motion steps must match them
    std::shared_ptr<groomlab::maya::NodeEvaluator> m_evaluator;  // kept across exports (keptEvaluator)
    std::optional<groomlab::LodPlan> m_lod;
    std::optional<groomlab::LodPlan> m_curvesPlan;  // when the curves leave out more than m_lod (curvesPlan)
    std::vector<std::string> m_declared;             // the curves' user data the last export declared
    bool m_heavyWarned = false;
    bool m_pauseWarned = false;
    AtNode* m_instancer = nullptr;  // the Instance node's instancer, when there is one
    groomlab::arnold::InstancerLayout m_instanceLayout;
    unsigned m_instanceKeys = 1;
    bool m_instancesWarned = false;
    AtNode* m_scales = nullptr;  // the scales' polymesh, when it takes motion keys
    std::shared_ptr<const groomlab::ScaleMesh> m_scaleMesh;  // its first key
    groomlab::ScaleMeshBuilder m_scaleBuilder;  // kept across the motion steps: bends only what moved
    std::map<std::string, groomlab::InstancePrototype> m_scenePrototypes;
    bool m_scalesWarned = false;
    AtNode* m_rachis = nullptr;          // the feathers' rachis polymesh, when it takes motion keys
    groomlab::RachisMesh m_rachisMesh;   // its first key
    bool m_rachisWarned = false;
};

}  // namespace

extern "C" {

DLLEXPORT void initializeExtension(CExtension& extension) {
    groomlab::maya::registerHostOperators();  // this DLL has its own operator registry
    extension.Requires("GroomLab");
    extension.RegisterTranslator("groomLabShape", "groomlab", GroomTranslator::creator,
                                 GroomTranslator::NodeInitializer);
    registerKeptCallbacks();
}

DLLEXPORT void deinitializeExtension(CExtension&) { removeKeptCallbacks(); }

}  // extern "C"
