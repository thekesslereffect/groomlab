// SPDX-License-Identifier: Apache-2.0
// V-Ray for Maya translator (vraytr_groomlab.dll): renders groomLabShape in V-Ray, as the MtoA
// extension does in Arnold. V-Ray loads translators named vraytr_*.dll from its translators folder
// and from VRAY_FOR_MAYA<year>_TRANSLATORS_PATH (GroomLab.mod adds the module's), and calls the
// one registered for a node type while it translates the scene (V-Ray for Maya's SDK:
// vray/include/vray_maya_sdk/translator_interface.h and its samples/maya_plugins/translators).
//
// Each groom becomes:
//   GeomMayaHair <groom>@groomlabHair   the strands at render density with the groom's render
//                                       overrides, in its object space (vrscene_writer.h
//                                       vray::hairArrays: what a .vrscene of it holds), the radii
//                                       times the transform's scale (V-Ray scales hair points by a
//                                       Node's transform, not its widths)
//   Node                                V-Ray's own (exportNodePlugin): the transform, render flags,
//                                       object IDs and light links as for any shape
//   material                            the groom's shading group's shader, translated by V-Ray (a
//                                       VRayHairNextMtl assigned in the Hypershade); without one
//                                       (none, or Maya's initialShadingGroup), GroomLab's hair:
//                                       BRDFHair4 reading the strands' colors through a
//                                       TexHairSampler, its lobes the groom's look (vray::vrayHairBrdf,
//                                       the .vrscene's material)
//
// The groom is evaluated by a NodeEvaluator kept per groom across frames and re-renders (as the
// MtoA extension keeps one), so a static groom's next frame is a cache hit.

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <maya/MAnimControl.h>
#include <maya/MCallbackIdArray.h>
#include <maya/MDGMessage.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MGlobal.h>
#include <maya/MMatrix.h>
#include <maya/MObjectArray.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MSceneMessage.h>
#include <maya/MTime.h>
#include <maya/MUuid.h>

// translator_interface.h first: it brings V-Ray's base headers (utils.h) the others assume;
// vrayplugins.h the parameter and texture interfaces defparams.h builds on.
#include "translator_interface.h"
#include "vrayplugins.h"
#include "defparams.h"
#include "plugman.h"

#include "groomlab/hair_color.h"
#include "groomlab/render_overrides.h"
#include "groomlab/vrscene_writer.h"
#include "maya_inputs.h"

namespace {

using Context = VRayMaya::MayaExporterContext;
using Exporter = VRay4MayaPluginInfo::MayaExporter9;

void warn(const MString& node, const std::string& what) {
    MGlobal::displayWarning("GroomLab: V-Ray: " + node + ": " + MString(what.c_str()));
}

// ---------------------------------------------------------------------------
// Evaluators kept from one translation to the next, one per groom (its node's UUID and instance):
// dropped when the node is deleted and on a new or opened scene.
std::mutex gKeptMutex;
std::map<std::string, std::shared_ptr<groomlab::maya::NodeEvaluator>> gKept;
MCallbackIdArray gCallbacks;

std::shared_ptr<groomlab::maya::NodeEvaluator> keptEvaluator(const MObject& node, unsigned instance) {
    std::lock_guard<std::mutex> lock(gKeptMutex);
    auto& kept = gKept[std::string(MFnDependencyNode(node).uuid().asString().asChar()) + "#" + std::to_string(instance)];
    if (!kept) kept = std::make_shared<groomlab::maya::NodeEvaluator>(node, true);
    return kept;
}

void dropKept(void* = nullptr) {
    std::lock_guard<std::mutex> lock(gKeptMutex);
    gKept.clear();
}

void dropKeptOfNode(MObject& node, void*) {
    if (MFnDependencyNode(node).typeName() != "groomLabShape") return;
    const std::string uuid = MFnDependencyNode(node).uuid().asString().asChar();
    std::lock_guard<std::mutex> lock(gKeptMutex);
    for (auto it = gKept.begin(); it != gKept.end();) {
        it = it->first.rfind(uuid + "#", 0) == 0 ? gKept.erase(it) : std::next(it);
    }
}

// ---------------------------------------------------------------------------
// Parameters

VUtils::VRaySettableParamInterface* settable(VUtils::VRayPluginParameter* param) {
    return param ? ::queryInterface<VUtils::VRaySettableParamInterface>(param) : nullptr;  // plugman.h
}

void setParam(Context& c, VUtils::VRayPlugin* plugin, VUtils::VRayPluginParameter* param) {
    if (plugin && param) c.mexporter6->setParameter(*plugin, param);
}

void setInt(Context& c, VUtils::VRayPlugin* plugin, const char* name, int value) {
    setParam(c, plugin, c.mexporter15->exportValueParam(name, value));
}

void setFloat(Context& c, VUtils::VRayPlugin* plugin, const char* name, float value) {
    setParam(c, plugin, c.mexporter15->exportValueParam(name, value));
}

void setPlugin(Context& c, VUtils::VRayPlugin* plugin, const char* name, VUtils::VRayPlugin* value,
               const char* output = nullptr) {
    setParam(c, plugin, c.mexporter6->pluginToParameter(name, value, output));
}

void setIntList(Context& c, VUtils::VRayPlugin* plugin, const char* name, const std::vector<std::int32_t>& values) {
    VUtils::VRayPluginParameter* param = c.mexporter9->createDefParam(name, Exporter::defIntListParam);
    VUtils::VRaySettableParamInterface* set = settable(param);
    if (!set) return;
    set->setCount(static_cast<int>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) set->setInt(values[i], static_cast<int>(i));
    setParam(c, plugin, param);
}

void setFloatList(Context& c, VUtils::VRayPlugin* plugin, const char* name, const std::vector<float>& values) {
    VUtils::VRayPluginParameter* param = c.mexporter9->createDefParam(name, Exporter::defFloatListParam);
    VUtils::VRaySettableParamInterface* set = settable(param);
    if (!set) return;
    set->setCount(static_cast<int>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) set->setFloat(values[i], static_cast<int>(i));
    setParam(c, plugin, param);
}

// `values`: 3 floats an element.
void setVectorList(Context& c, VUtils::VRayPlugin* plugin, const char* name, const std::vector<float>& values) {
    VUtils::VRayPluginParameter* param = c.mexporter9->createDefParam(name, Exporter::defVectorListParam);
    VUtils::VRaySettableParamInterface* set = settable(param);
    if (!set) return;
    const std::size_t n = values.size() / 3;
    set->setCount(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) {
        set->setVector(VUtils::Vector(values[3 * i], values[3 * i + 1], values[3 * i + 2]), static_cast<int>(i));
    }
    setParam(c, plugin, param);
}

void setColorList(Context& c, VUtils::VRayPlugin* plugin, const char* name, const std::vector<float>& values) {
    VUtils::VRayPluginParameter* param = c.mexporter9->createDefParam(name, Exporter::defColorListParam);
    VUtils::VRaySettableParamInterface* set = settable(param);
    if (!set) return;
    const std::size_t n = values.size() / 3;
    set->setCount(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) {
        set->setColor(VUtils::Color(values[3 * i], values[3 * i + 1], values[3 * i + 2]), static_cast<int>(i));
    }
    setParam(c, plugin, param);
}

VUtils::VRayPlugin* createPlugin(Context& c, const char* type, const VUtils::CharString& name) {
    bool existing = false;
    return c.mexporter9->createPlugin(type, name.ptr(), existing, c.flags.recreate != 0);
}

// ---------------------------------------------------------------------------
// The groom's shading group's surface shader, when it has one of its own (not Maya's default).
MPlug assignedShader(const MDagPath& path) {
    MFnDagNode shape(path);
    MPlug groups = shape.findPlug("instObjGroups", true);
    if (groups.isNull() || groups.numElements() == 0) return MPlug();
    MPlug group = groups.elementByLogicalIndex(path.instanceNumber());
    MPlugArray targets;
    if (!group.connectedTo(targets, false, true)) return MPlug();
    for (unsigned i = 0; i < targets.length(); ++i) {
        MFnDependencyNode sg(targets[i].node());
        if (sg.typeName() != "shadingEngine" || sg.name() == "initialShadingGroup") continue;
        MPlug shader = sg.findPlug("surfaceShader", true);
        if (!shader.isNull() && shader.isDestination()) return shader;
    }
    return MPlug();
}

class GroomTranslator : public VRayMaya::InstanceTranslator {
public:
    VUtils::VRayPlugin* translateInstance(Context& c, int /*mbSamples*/,
                                          Context::OutFlags& outFlags) override {
        outFlags.existing = false;
        if (!c.mexporter15) {
            MGlobal::displayError("GroomLab: this V-Ray for Maya is too old for GroomLab's translator");
            return nullptr;
        }
        MFnDependencyNode depNode(c.object);
        const MString nodeName = c.path.partialPathName();
        try {  // never let an exception reach V-Ray (a render-density bad_alloc, say)
            groomlab::maya::refreshMayaPaths();  // this DLL keeps its own copy
            const auto evaluator = keptEvaluator(c.object, c.path.instanceNumber());
            evaluator->refreshStoredInputs();  // edits since its last translation
            groomlab::RenderOverrides overrides;
            overrides.density = depNode.findPlug("renderDensity", true).asFloat();
            overrides.widthScale = depNode.findPlug("renderWidthScale", true).asFloat();
            overrides.lengthScale = depNode.findPlug("renderLengthScale", true).asFloat();
            std::string error;
            groomlab::MeshPtr scalp;
            const groomlab::StrandsPtr strands =
                groomlab::applyRenderOverrides(evaluator->evaluate(overrides.densityTimes(1.0f), &error, &scalp), overrides);
            if (!error.empty()) warn(nodeName, error);
            if (!strands) return nullptr;

            // The Node's transform scales the points but not V-Ray's hair widths: the radii take it.
            const MMatrix world = c.path.inclusiveMatrix();
            double m[16];
            for (int r = 0; r < 4; ++r) {
                for (int k = 0; k < 4; ++k) m[4 * r + k] = world[r][k];
            }
            groomlab::vray::HairArrays a = groomlab::vray::hairArrays(*strands, scalp.get());
            const float widthScale = static_cast<float>(groomlab::vray::matrixScale(m));
            for (float& w : a.widths) w *= widthScale;

            const VUtils::CharString hairName = c.mexporter9->makeGeometryName(depNode, "@groomlabHair");
            VUtils::VRayPlugin* hair = createPlugin(c, "GeomMayaHair", hairName);
            if (!hair) return nullptr;
            setIntList(c, hair, "num_hair_vertices", a.counts);
            setVectorList(c, hair, "hair_vertices", a.xyz);
            setFloatList(c, hair, "widths", a.widths);
            setColorList(c, hair, "colors", a.colors);
            if (a.hasTransparency) setColorList(c, hair, "transparency", a.transparency);
            if (a.hasUVs) setVectorList(c, hair, "strand_uvw", a.uvw);
            setInt(c, hair, "geom_splines", 1);

            VUtils::VRayPlugin* node = c.mexporter9->exportNodePlugin(c.path, hair, c.flags.recreate != 0);
            if (!node) return nullptr;
            const MPlug shader = assignedShader(c.path);
            VUtils::VRayPluginParameter* material =
                shader.isNull() ? nullptr : c.mexporter6->exportParam(shader, "material", Exporter::PT_material, 0, true);
            if (!material) {
                groomlab::HairLook look;
                if (!evaluator->hairLook(&look)) look = groomlab::HairLook{};
                material = c.mexporter6->pluginToParameter("material", hairMaterial(c, depNode, look, a.hasTransparency));
            }
            setParam(c, node, material);
            return node;
        } catch (const std::exception& e) {
            MGlobal::displayError("GroomLab: V-Ray: " + nodeName + ": " + MString(e.what()));
            return nullptr;
        }
    }

private:
    // GroomLab's hair for a groom without a shader of its own: the .vrscene's (vrscene_writer.h).
    VUtils::VRayPlugin* hairMaterial(Context& c, MFnDependencyNode& depNode, const groomlab::HairLook& look,
                                     bool transparency) {
        const groomlab::vray::HairBrdf b = groomlab::vray::vrayHairBrdf(groomlab::hairLobes(look));
        VUtils::VRayPlugin* sampler = createPlugin(c, "TexHairSampler", c.mexporter9->makeGeometryName(depNode, "@groomlabHairSampler"));
        VUtils::VRayPlugin* brdf = createPlugin(c, "BRDFHair4", c.mexporter9->makeGeometryName(depNode, "@groomlabHairBrdf"));
        VUtils::VRayPlugin* mtl = createPlugin(c, "MtlSingleBRDF", c.mexporter9->makeGeometryName(depNode, "@groomlabHairMtl"));
        if (!sampler || !brdf || !mtl) return mtl;
        setFloat(c, brdf, "melanin", 0.0f);  // the pigment is in the colors
        setFloat(c, brdf, "pheomelanin", 0.0f);
        setPlugin(c, brdf, "dye_color", sampler, "hair_color");
        setFloat(c, brdf, "glossiness", b.glossiness);
        setFloat(c, brdf, "softness", b.softness);
        setFloat(c, brdf, "primary_glossiness_boost", 0.0f);
        setParam(c, brdf, c.mexporter15->exportValueParam("primary_tint", VUtils::AColor(b.primaryTint, b.primaryTint, b.primaryTint, 1.0f)));
        setFloat(c, brdf, "highlight_shift", b.highlightShift);
        setFloat(c, brdf, "ior", b.ior);
        setInt(c, brdf, "trace_depth", b.traceDepth);
        setFloat(c, brdf, "glint_strength", 0.0f);
        setFloat(c, brdf, "glitter_strength", 0.0f);
        if (transparency) setPlugin(c, brdf, "transparency", sampler, "hair_transparency");
        setPlugin(c, mtl, "brdf", brdf);
        return mtl;
    }
};

VRayMaya::InstanceTranslator* creator() { return new GroomTranslator(); }
void deleter(VRayMaya::InstanceTranslator* t) { delete t; }

}  // namespace

VMAYAPT_EXPORT
void initializeTranslators(VRayMaya::IRegistrator& reg) {
    groomlab::maya::registerHostOperators();  // this DLL has its own operator registry
    reg.reg("groomLabShape", creator, deleter);
    MStatus status;
    gCallbacks.append(MDGMessage::addNodeRemovedCallback(dropKeptOfNode, "groomLabShape", nullptr, &status));
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kBeforeNew, dropKept, nullptr, &status));
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kBeforeOpen, dropKept, nullptr, &status));
}

VMAYAPT_EXPORT
void uninitializeTranslators(VRayMaya::IRegistrator& reg) {
    reg.dereg("groomLabShape");
    if (gCallbacks.length() > 0) MMessage::removeCallbacks(gCallbacks);
    gCallbacks.clear();
    dropKept();
}
