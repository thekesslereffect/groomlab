// SPDX-License-Identifier: Apache-2.0
// Arnold procedural (OR-3): renders a groom file at render time, without
// Maya. It evaluates the groom at the frame (and at each motion key when the
// node has a shutter, OR-5) and creates one curves node.
//
//   groomlab_procedural
//   {
//     name hair
//     filename "C:/shots/010/hair.groom"
//     frame 12
//     motion_start -0.25  motion_end 0.25  motion_keys 3
//     shader "hairShader"
//   }
//
// Instance nodes' geometry comes out as an instancer beside the curves, with
// an instance_matrix key per motion key (deformation blur for the instances too),
// and scales as one polymesh with a key of points and normals per motion key.
// Prototype meshes come from the groom file (GroomFile::prototypes: the scene
// meshes it was written with), else from the scene by name.
//
// Parameters:
//   filename         the .groom file (its scalp cache is found next to it)
//   frame            the frame to render; times are frame / fps
//   fps              frames per second; 0 uses the groom file's
//   density          fraction of strands to keep, by stable strand ID
//   motion_keys      keys across [motion_start, motion_end] (frames, relative to `frame`)
//   mode             curves mode: ribbon or thick (Arnold has no oriented mode for linear curves)
//   min_pixel_width  passed to the curves
//   width_scale      multiplies strand widths
//   length_scale     scales each strand about its root (render_overrides.h: Maya's renderLengthScale)
//   lod_distance, lod_near, lod_far, lod_min_density, lod_width_compensation
//                    thin strands with distance from the render camera (lod.h)
//   camera_cull, cull_margin
//                    drop strands outside the render camera's view
//   backface_cull, backface_angle
//                    drop strands rooted facing away from the render camera (the angle: degrees
//                    of padding past side-on)
//
// The shape parameters every procedural has (shader, visibility, sidedness,
// matte, opaque, the shadow flags, light and shadow groups, trace sets, id,
// transform_type) pass to the curves. The curves carry user data: strand_id
// (uniform UINT, low 32 bits of the stable ID), every strand channel (uniform
// FLOAT, e.g. clumpId), hair_color (uniform RGB) when the groom has HairColor,
// plus the root UV in `uvs` when the scalp has UVs.
//
// Memory: the graph keeps no intermediate results, each motion key streams
// straight into the curves' points array, and the groom scene is gone before
// Arnold copies the arrays.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ai.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "groomlab/alembic_operators.h"
#include "groomlab/arnold_curves.h"
#include "groomlab/export_view.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/groom_scene.h"
#include "groomlab/lod.h"
#include "groomlab/paths.h"
#include "groomlab/render_overrides.h"
#include "groomlab/scales.h"

namespace {

struct ProcData {
    std::vector<AtNode*> nodes;
};

#if defined(_WIN32) && defined(GROOMLAB_TBB_DLL)
// TBB is delay-loaded (see CMakeLists.txt): load it from this plugin's folder
// unless the host already has it, before anything calls into it.
void loadRuntimeDlls() {
    if (GetModuleHandleW(GROOMLAB_TBB_DLL)) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&loadRuntimeDlls), &self);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    if (!LoadLibraryExW((dir + GROOMLAB_TBB_DLL).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        AiMsgWarning("[groomlab] cannot load %ls beside the procedural; put it on PATH", GROOMLAB_TBB_DLL);
    }
}
#else
void loadRuntimeDlls() {}
#endif

const char* kModes[] = {"ribbon", "thick", nullptr};

// The curves node for the first motion key: geometry with `keys` point keys
// (later keys are streamed in by setPointsKey) and the procedural's shape
// parameters.
AtNode* createCurves(AtNode* proc, const groomlab::ExportView& view, unsigned keys) {
    const std::string name = std::string(AiNodeGetName(proc)) + "/strands";
    AtNode* curves = AiNode(AiNodeGetUniverse(proc), AtString("curves"), AtString(name.c_str()), proc);
    groomlab::arnold::setCurves(curves, view, keys, AiNodeGetFlt(proc, AtString("width_scale")));
    AiNodeSetStr(curves, AtString("mode"), AtString(groomlab::arnold::curvesMode(AiNodeGetInt(proc, AtString("mode")))));
    AiNodeSetFlt(curves, AtString("min_pixel_width"), AiNodeGetFlt(proc, AtString("min_pixel_width")));
    AiNodeSetFlt(curves, AtString("motion_start"), AiNodeGetFlt(proc, AtString("motion_start")));
    AiNodeSetFlt(curves, AtString("motion_end"), AiNodeGetFlt(proc, AtString("motion_end")));
    groomlab::arnold::copyShapeParameters(proc, curves);
    return curves;
}

// A prototype mesh in the scene by the name the Instance node gives: the exact
// node name, or a shape whose DAG-style name ends in /name, |name or nameShape.
AtNode* findPrototype(AtUniverse* universe, const std::string& name) {
    if (AtNode* exact = AiNodeLookUpByName(universe, AtString(name.c_str()))) return exact;
    AtNode* found = nullptr;
    AtNodeIterator* it = AiUniverseGetNodeIterator(universe, AI_NODE_SHAPE);
    while (!AiNodeIteratorFinished(it) && !found) {
        AtNode* n = AiNodeIteratorGetNext(it);
        const std::string full = AiNodeGetName(n);
        for (const std::string& tail : {"|" + name, "/" + name, "|" + name + "Shape", "/" + name + "Shape", name + "Shape"}) {
            if (full.size() >= tail.size() && full.compare(full.size() - tail.size(), tail.size(), tail) == 0) {
                found = n;
                break;
            }
        }
    }
    AiNodeIteratorDestroy(it);
    return found;
}

// The instancer for the first key, with `keys` instance_matrix keys (later
// keys are filled by setInstancerKey); null when there is nothing to instance.
AtNode* buildInstancer(AtNode* proc, const groomlab::StrandSet& strands, unsigned keys,
                       groomlab::arnold::InstancerLayout* layout, bool warn, const groomlab::PrototypeLookup& stored,
                       std::vector<AtNode*>* made) {
    const auto& instances = strands.instances();
    if (!instances || instances->empty()) return nullptr;
    // The prototypes instances on strands take (scales are buildScales').
    std::vector<char> used(instances->prototypes.size(), 0);
    for (const auto& i : instances->instances) {
        if (!i.meshed() && i.prototype < used.size()) used[i.prototype] = 1;
    }
    if (std::find(used.begin(), used.end(), 1) == used.end()) return nullptr;
    AtUniverse* universe = AiNodeGetUniverse(proc);
    std::vector<AtNode*> prototypes;
    for (std::size_t p = 0; p < instances->prototypes.size(); ++p) {
        const std::string& name = instances->prototypes[p];
        AtNode* proto = nullptr;
        if (!used[p]) {
        } else if (const groomlab::InstancePrototype* own = groomlab::builtinPrototype(name) ? groomlab::builtinPrototype(name) : stored(name)) {
            // A built-in plate, or a mesh the groom file holds.
            const std::string nodeName = std::string(AiNodeGetName(proc)) + "/prototype" + std::to_string(p);
            proto = AiNodeLookUpByName(universe, AtString(nodeName.c_str()), proc);  // (made by an earlier build)
            if (!proto) {
                proto = AiNode(universe, AtString("polymesh"), AtString(nodeName.c_str()), proc);
                groomlab::arnold::setPrototypeMesh(proto, *own);
                AiNodeSetByte(proto, AtString("visibility"), 0);  // only its instances render
                if (AtNode* inScene = findPrototype(universe, name)) {  // its shader, when the scene has the mesh
                    const AtArray* shader = AiNodeGetArray(inScene, AtString("shader"));
                    if (shader && AiArrayGetNumElements(shader) > 0) groomlab::arnold::setShader(proto, static_cast<AtNode*>(AiArrayGetPtr(shader, 0)));
                }
                made->push_back(proto);
            }
        } else {
            proto = findPrototype(universe, name);
            if (!proto && warn) AiMsgWarning("[groomlab] %s: no prototype '%s' in the scene to instance", AiNodeGetName(proc), name.c_str());
        }
        prototypes.push_back(proto);
    }
    const std::string name = std::string(AiNodeGetName(proc)) + "/instances";
    AtNode* instancer = AiNode(universe, AtString("instancer"), AtString(name.c_str()), proc);
    if (!groomlab::arnold::setInstancer(instancer, prototypes, strands, keys, layout)) {
        AiNodeDestroy(instancer);
        return nullptr;
    }
    AiNodeSetFlt(instancer, AtString("motion_start"), AiNodeGetFlt(proc, AtString("motion_start")));
    AiNodeSetFlt(instancer, AtString("motion_end"), AiNodeGetFlt(proc, AtString("motion_end")));
    return instancer;
}

// The scales' polymesh and what its later motion keys are made with.
struct Scales {
    AtNode* polymesh = nullptr;
    std::shared_ptr<const groomlab::ScaleMesh> first;  // the first key's
    groomlab::ScaleMeshBuilder builder;               // kept across keys: bends only what moved
    // Every key the first one (the scales did not move, or changed between keys).
    void holdStill() {
        const AtArray* points = polymesh ? AiNodeGetArray(polymesh, AtString("vlist")) : nullptr;
        for (unsigned k = 1; points && k < AiArrayGetNumKeys(points); ++k) groomlab::arnold::setScaleMeshKey(polymesh, *first, *first, k);
    }
};

// The scales (scales.h) bent onto `scalp` as one polymesh with their user data (setScaleMesh),
// with `keys` motion keys of points and normals (each the first; later keys are set from their
// own strands and skin): each takes its Instance node's material (a shader in the scene by
// name), else its prototype mesh's shader, else a standard_surface reading scaleColor. The
// prototypes: the built-in plates, the groom file's meshes (`stored`). The nodes made go into
// `made`.
void buildScales(AtNode* proc, const groomlab::StrandSet& strands, const groomlab::MeshPtr& scalp,
                 const groomlab::PrototypeLookup& stored, unsigned keys, Scales* out, std::vector<AtNode*>* made) {
    const auto& instances = strands.instances();
    if (!instances || !instances->hasMeshes()) return;
    AtUniverse* universe = AiNodeGetUniverse(proc);
    const std::string procName = AiNodeGetName(proc);
    const auto mesh = out->builder.build(strands, scalp, stored);
    if (!mesh || mesh->triangles.empty()) return;
    std::vector<AtNode*> shaders;
    std::map<std::pair<std::int32_t, std::uint32_t>, std::uint8_t> slotOf;
    std::vector<std::uint8_t> slots(mesh->scales.size(), 0);
    AtNode* fallback = nullptr;
    for (std::size_t k = 0; k < mesh->scales.size(); ++k) {
        const groomlab::ScaleMesh::Scale& s = mesh->scales[k];
        const auto key = std::make_pair(s.material, s.prototype);
        auto it = slotOf.find(key);
        if (it == slotOf.end()) {
            AtNode* shader = nullptr;
            if (s.material >= 0 && static_cast<std::size_t>(s.material) < instances->materials.size()) {
                const std::string& name = instances->materials[static_cast<std::size_t>(s.material)];
                shader = AiNodeLookUpByName(universe, AtString(name.c_str()));
                if (!shader) AiMsgWarning("[groomlab] %s: no shader '%s' for the scales", procName.c_str(), name.c_str());
            }
            const std::string& prototype = s.prototype < instances->prototypes.size() ? instances->prototypes[s.prototype] : "";
            if (!shader && !groomlab::builtinPrototype(prototype)) {
                if (AtNode* proto = findPrototype(universe, prototype)) {
                    const AtArray* own = AiNodeGetArray(proto, AtString("shader"));
                    if (own && AiArrayGetNumElements(own) > 0) shader = static_cast<AtNode*>(AiArrayGetPtr(own, 0));
                }
            }
            if (!shader) {
                if (!fallback) {
                    fallback = AiNode(universe, AtString("standard_surface"), AtString((procName + "/scaleSurface").c_str()), proc);
                    AtNode* data = AiNode(universe, AtString("user_data_rgb"), AtString((procName + "/scaleColor").c_str()), proc);
                    groomlab::arnold::setDefaultScaleShader(fallback, data);
                    made->push_back(fallback);
                    made->push_back(data);
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
    AtNode* polymesh = AiNode(universe, AtString("polymesh"), AtString((procName + "/scales").c_str()), proc);
    groomlab::arnold::setScaleMesh(polymesh, *mesh, shaders, slots, keys);
    for (const char* name : {"visibility", "matte", "opaque", "receive_shadows", "self_shadows", "trace_sets", "id",
                             "motion_start", "motion_end"}) {
        groomlab::arnold::copyParameter(proc, polymesh, name);
    }
    made->push_back(polymesh);
    out->polymesh = polymesh;
    out->first = mesh;
    AiMsgInfo("[groomlab] %s: %zu scales, %zu triangles", procName.c_str(), mesh->scales.size(), mesh->triangleCount());
}

// The feathers' rachis (a Propagate node's shaftWidth; feathers.h): their tubes as one polymesh
// (setRachisMesh) with `keys` motion keys of points (each the first; later keys are set from their
// own strands), rendered with a standard_surface reading each feather's shaft color. The shafts
// themselves stay out of the curves (curvesPlan).
struct Rachis {
    AtNode* polymesh = nullptr;
    groomlab::RachisMesh first;
    void holdStill() {
        const AtArray* points = polymesh ? AiNodeGetArray(polymesh, AtString("vlist")) : nullptr;
        for (unsigned k = 1; points && k < AiArrayGetNumKeys(points); ++k) groomlab::arnold::setRachisMeshKey(polymesh, first, first, k);
    }
};

void buildRachis(AtNode* proc, const groomlab::StrandSet& strands, unsigned keys, Rachis* out, std::vector<AtNode*>* made) {
    if (!groomlab::hasRachis(strands.featherLayers())) return;
    groomlab::RachisMesh mesh = groomlab::buildRachisMesh(strands.featherLayers());
    if (mesh.faceCount() == 0) return;
    AtUniverse* universe = AiNodeGetUniverse(proc);
    const std::string procName = AiNodeGetName(proc);
    AtNode* surface = AiNode(universe, AtString("standard_surface"), AtString((procName + "/rachisSurface").c_str()), proc);
    AtNode* data = AiNode(universe, AtString("user_data_rgb"), AtString((procName + "/rachisColor").c_str()), proc);
    groomlab::arnold::setDefaultRachisShader(surface, data);
    AtNode* polymesh = AiNode(universe, AtString("polymesh"), AtString((procName + "/rachis").c_str()), proc);
    groomlab::arnold::setRachisMesh(polymesh, mesh, groomlab::rachisColors(strands, mesh, {0.5f, 0.45f, 0.4f}), keys);
    groomlab::arnold::setShader(polymesh, surface);
    for (const char* name : {"visibility", "matte", "opaque", "receive_shadows", "self_shadows", "trace_sets", "id",
                             "motion_start", "motion_end"}) {
        groomlab::arnold::copyParameter(proc, polymesh, name);
    }
    made->insert(made->end(), {surface, data, polymesh});
    out->polymesh = polymesh;
    AiMsgInfo("[groomlab] %s: %zu feathers' rachis, %zu faces", procName.c_str(), mesh.featherCount, mesh.faceCount());
    out->first = std::move(mesh);
}

// Render LOD against the options' camera, planned on the first key: the plan
// applies to every key, so they keep the same strands. None when LOD is off
// or there is no camera.
std::optional<groomlab::LodPlan> planLod(AtNode* proc, const groomlab::StrandSet& strands, const groomlab::ScalpMesh* scalp) {
    groomlab::LodSettings s;
    s.distance = AiNodeGetBool(proc, AtString("lod_distance"));
    s.frustum = AiNodeGetBool(proc, AtString("camera_cull"));
    s.backface = AiNodeGetBool(proc, AtString("backface_cull"));
    if ((!s.distance && !s.frustum && !s.backface) || strands.empty()) return std::nullopt;
    s.backfaceAngle = AiNodeGetFlt(proc, AtString("backface_angle"));
    s.nearDistance = AiNodeGetFlt(proc, AtString("lod_near"));
    s.farDistance = AiNodeGetFlt(proc, AtString("lod_far"));
    s.minDensity = AiNodeGetFlt(proc, AtString("lod_min_density"));
    s.widthCompensation = AiNodeGetFlt(proc, AtString("lod_width_compensation"));
    s.margin = AiNodeGetFlt(proc, AtString("cull_margin"));
    AtUniverse* universe = AiNodeGetUniverse(proc);
    AtNode* camera = AiUniverseGetCamera(universe);
    groomlab::LodCamera cam;
    if (!groomlab::arnold::lodCameraFromArnold(camera, AiUniverseGetOptions(universe), AiNodeGetMatrix(proc, AtString("matrix")), &cam)) {
        AiMsgWarning("[groomlab] %s: LOD needs a render camera; rendering every strand", AiNodeGetName(proc));
        return std::nullopt;
    }
    groomlab::LodPlan plan = groomlab::planLod(strands, cam, s, scalp);
    AiMsgInfo("[groomlab] %s: LOD keeps %zu of %zu strands (%zu outside the view, %zu facing away, %zu thinned)",
              AiNodeGetName(proc), plan.kept.size(), strands.strandCount(), plan.culled, plan.backfacing, plan.thinned);
    return plan;
}

}  // namespace

AI_PROCEDURAL_NODE_EXPORT_METHODS(GroomLabProceduralMethods);

node_parameters {
    AiParameterStr("filename", "");
    AiParameterStr("groom", "");  // the groom of a file of several, by name ("" the first)
    AiParameterFlt("frame", 1.0f);
    AiParameterFlt("fps", 0.0f);
    AiParameterFlt("density", 1.0f);
    AiParameterInt("motion_keys", 2);
    AiParameterEnum("mode", 0, kModes);
    AiParameterFlt("min_pixel_width", 0.0f);
    AiParameterFlt("width_scale", 1.0f);
    AiParameterFlt("length_scale", 1.0f);
    AiParameterBool("lod_distance", false);
    AiParameterFlt("lod_near", 10.0f);
    AiParameterFlt("lod_far", 100.0f);
    AiParameterFlt("lod_min_density", 0.1f);
    AiParameterFlt("lod_width_compensation", 1.0f);
    AiParameterBool("camera_cull", false);
    AiParameterFlt("cull_margin", 0.1f);
    AiParameterBool("backface_cull", false);
    AiParameterFlt("backface_angle", 20.0f);
}

procedural_init {
    auto data = std::make_unique<ProcData>();
    const char* procName = AiNodeGetName(node);
    const std::string filename = groomlab::expandEnvironment(AiNodeGetStr(node, AtString("filename")).c_str());
    if (filename.empty()) {
        AiMsgError("[groomlab] %s: no filename", procName);
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
        auto scene = std::make_unique<groomlab::GroomScene>(filename, AiNodeGetStr(node, AtString("groom")).c_str());
        const float fpsParam = AiNodeGetFlt(node, AtString("fps"));
        // The param is a float: the file's own rate when it is that rate. Seconds by every host's
        // rule (frame_time.h: an NTSC rate exact), so a frame is the CLI's and Maya's to the bit.
        const double fps = fpsParam > 0.0f && fpsParam != static_cast<float>(scene->framesPerSecond()) ? fpsParam
                                                                                                        : scene->framesPerSecond();
        const double frame = AiNodeGetFlt(node, AtString("frame"));
        const float density = AiClamp(AiNodeGetFlt(node, AtString("density")), 0.0f, 1.0f);
        // Strands scaled about their roots (the width scale goes to the curves' radii).
        groomlab::RenderOverrides lengths;
        lengths.lengthScale = AiNodeGetFlt(node, AtString("length_scale"));
        auto evaluateAt = [&](double seconds) { return groomlab::applyRenderOverrides(scene->evaluate(seconds, density), lengths); };
        const float motionStart = AiNodeGetFlt(node, AtString("motion_start"));
        const float motionEnd = AiNodeGetFlt(node, AtString("motion_end"));
        const int requestedKeys = AiNodeGetInt(node, AtString("motion_keys"));
        const unsigned keyCount = motionEnd > motionStart && requestedKeys >= 2 ? static_cast<unsigned>(AiMin(requestedKeys, 255)) : 1u;
        // One key is one shot: nothing is re-evaluated, keep only the result. Motion keys keep the
        // graph's cache, so each key re-evaluates only what moves (as the MtoA translator does).
        scene->setCacheIntermediates(keyCount > 1);
        auto keyTime = [&](unsigned k) {
            const double offset = keyCount == 1 ? 0.0 : motionStart + (motionEnd - motionStart) * static_cast<double>(k) / (keyCount - 1);
            return groomlab::frameToSeconds(frame + offset, fps);
        };

        const groomlab::MeshPtr scalp = scene->rootScalpAt(groomlab::frameToSeconds(frame, fps));
        // The first key, as rendered (after LOD): its topology (IDs and
        // offsets) is what every other key must match. Later keys stream into
        // the points array one at a time and are dropped.
        groomlab::StrandsPtr first = evaluateAt(keyTime(0));
        if (!first) first = std::make_shared<groomlab::StrandSet>();
        // A groom that nothing moves (GroomScene::readsTime): every key is the first one, so the
        // other keys are not evaluated (the same values go into them).
        const groomlab::StrandsPtr still = scene->readsTime() ? nullptr : first;
        std::vector<groomlab::StrandId> fullIds = first->ids();
        std::vector<std::size_t> fullOffsets = first->offsets();
        const std::optional<groomlab::LodPlan> lod = planLod(node, *first, scalp.get());
        // The curves take the strands Arnold can render (no fewer than two points, no point that is
        // not finite: curvesPlan, as the MtoA translator's); the instances and scales lay out from all.
        std::size_t unrenderable = 0;
        const std::optional<groomlab::LodPlan> curvesPlan = groomlab::arnold::curvesPlan(*first, lod ? &*lod : nullptr, &unrenderable);
        if (unrenderable > 0) {
            AiMsgWarning("[groomlab] %s: %zu strands with fewer than two points or a point that is not a number are left "
                         "out of the curves",
                         procName, unrenderable);
        }
        const groomlab::LodPlan* pointsPlan = curvesPlan ? &*curvesPlan : lod ? &*lod : nullptr;
        const groomlab::StrandsPtr curveStrands =
            curvesPlan ? std::make_shared<groomlab::StrandSet>(groomlab::applyLod(*first, *curvesPlan)) : nullptr;
        if (lod) first = std::make_shared<groomlab::StrandSet>(groomlab::applyLod(*first, *lod));
        groomlab::ExportView view =
            groomlab::describeExport(curveStrands ? *curveStrands : *first, scalp.get(), groomlab::kExportPointColors);

        AtNode* curves = nullptr;
        unsigned keys = keyCount;
        if (view.strandCount() > 0) curves = createCurves(node, view, keys);
        // The instances get as many keys as the curves, from each key's strands.
        groomlab::arnold::InstancerLayout layout;
        // The groom file's prototype meshes (the scene meshes it was written with).
        const std::map<std::string, groomlab::InstancePrototype>& storedPrototypes = scene->file().prototypes;
        const groomlab::PrototypeLookup stored = [&](const std::string& name) -> const groomlab::InstancePrototype* {
            const auto it = storedPrototypes.find(name);
            return it != storedPrototypes.end() && it->second.found ? &it->second : nullptr;
        };
        AtNode* instancer = buildInstancer(node, *first, keys, &layout, true, stored, &data->nodes);
        Scales scales;
        buildScales(node, *first, scalp, stored, still ? 1u : keys, &scales, &data->nodes);  // (still: one key is all keys)
        Rachis rachis;
        buildRachis(node, *first, still ? 1u : keys, &rachis, &data->nodes);
        bool keyRachis = rachis.polymesh && keys > 1 && !still;
        bool keyInstances = instancer && keys > 1;
        bool keyScales = scales.polymesh && keys > 1 && !still;  // (nothing moves: every key is the first)
        auto staticInstancer = [&] {  // one key: the first
            keyInstances = false;
            if (!instancer) return;
            AiNodeDestroy(instancer);
            instancer = buildInstancer(node, *first, 1, nullptr, false, stored, &data->nodes);
        };
        auto staticScales = [&] {
            keyScales = false;
            scales.holdStill();
        };
        auto staticRachis = [&] {
            keyRachis = false;
            rachis.holdStill();
        };
        for (unsigned k = 1; k < keys && curves; ++k) {
            groomlab::StrandsPtr strands = still ? still : evaluateAt(keyTime(k));
            if (!strands || strands->ids() != fullIds || strands->offsets() != fullOffsets) {
                AiMsgWarning(
                    "[groomlab] %s: strands change between motion keys (give the groom a rest mesh); rendering "
                    "without deformation blur",
                    procName);
                AiNodeDestroy(curves);
                keys = 1;
                curves = createCurves(node, view, keys);
                staticInstancer();
                staticScales();
                staticRachis();
                break;
            }
            groomlab::arnold::setPointsKey(curves, *strands, k, pointsPlan);
            if (keyInstances && !groomlab::arnold::setInstancerKey(instancer, layout, *strands, k)) {
                AiMsgWarning("[groomlab] %s: instances change between motion keys; rendering them without deformation blur",
                             procName);
                staticInstancer();
            }
            if (keyScales) {
                // This key's scales: its strands (as rendered, through the LOD plan) on its skin.
                const groomlab::StrandSet kept = lod ? groomlab::applyLod(*strands, *lod) : *strands;
                const auto mesh = scales.builder.build(kept, scene->rootScalpAt(keyTime(k)), stored);
                if (!mesh || !groomlab::arnold::setScaleMeshKey(scales.polymesh, *scales.first, *mesh, k)) {
                    AiMsgWarning("[groomlab] %s: scales change between motion keys; rendering them without deformation blur",
                                 procName);
                    staticScales();
                }
            }
            if (keyRachis && !groomlab::arnold::setRachisMeshKey(rachis.polymesh, rachis.first,
                                                                 groomlab::buildRachisMesh(strands->featherLayers()), k)) {
                AiMsgWarning("[groomlab] %s: the feathers' rachis changes between motion keys; rendering it without "
                             "deformation blur",
                             procName);
                staticRachis();
            }
        }
        fullIds.clear();
        fullOffsets.clear();
        scene.reset();  // the graph and caches go before Arnold copies the arrays

        if (curves) {
            groomlab::arnold::setStrandData(curves, view, scalp.get());
            // Strands that only lay scales out render as the scales alone.
            if (first->instances() && first->instances()->hidesStrands) AiNodeSetByte(curves, AtString("visibility"), 0);
            data->nodes.push_back(curves);
        }
        if (instancer) data->nodes.push_back(instancer);
        // A shader set on the procedural overrides its shapes' own (Arnold's inheritance): the
        // curves hold a copy of it, so with scales (each with a shader of its own) it goes.
        if ((first->instances() && first->instances()->hasMeshes()) || rachis.polymesh) AiNodeResetParameter(node, AtString("shader"));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        AiMsgInfo("[groomlab] %s: %zu strands, %zu points, %u motion key(s) from %s at frame %g (%.0f ms)", procName,
                  first->strandCount(), first->pointCount(), keys, filename.c_str(), frame, ms);
    } catch (const std::exception& e) {
        AiMsgError("[groomlab] %s: %s", procName, e.what());
        return 0;
    }
    *user_ptr = data.release();
    return 1;
}

procedural_cleanup {
    delete static_cast<ProcData*>(user_ptr);
    return 1;
}

procedural_num_nodes {
    const auto* data = static_cast<const ProcData*>(user_ptr);
    return data ? static_cast<int>(data->nodes.size()) : 0;
}

procedural_get_node {
    const auto* data = static_cast<const ProcData*>(user_ptr);
    return data && i >= 0 && static_cast<std::size_t>(i) < data->nodes.size() ? data->nodes[static_cast<std::size_t>(i)]
                                                                                  : nullptr;
}

node_loader {
    if (i > 0) return false;
    static const bool registered = [] {
        loadRuntimeDlls();
        groomlab::registerAlembicOperators(groomlab::OperatorRegistry::instance());
        return true;
    }();
    (void)registered;
    node->methods = GroomLabProceduralMethods;
    node->output_type = AI_TYPE_NONE;
    node->name = "groomlab_procedural";
    node->node_type = AI_NODE_SHAPE_PROCEDURAL;
    std::strcpy(node->version, AI_VERSION);
    return true;
}
