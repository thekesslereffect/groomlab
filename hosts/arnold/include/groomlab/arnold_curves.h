// SPDX-License-Identifier: Apache-2.0
#pragma once

// Strands to an Arnold curves node, shared by the MtoA translator and the
// procedural so both render the same thing: the curves' geometry and user
// data (from export_view.h, as every other exporter), the shape parameters a
// procedural passes to its curves, and the render camera as an LOD camera.
// Header-only: each binary links Arnold on its own.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <ai.h>

#include "groomlab/color_patterns.h"
#include "groomlab/export_view.h"
#include "groomlab/lod.h"
#include "groomlab/portable_math.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::arnold {

static_assert(sizeof(AtVector) == sizeof(Vec3f), "AtVector and Vec3f must share a layout");

// Same strands and point counts: needed to blend motion keys.
inline bool sameTopology(const StrandSet& a, const StrandSet& b) {
    return a.strandCount() == b.strandCount() && a.pointCount() == b.pointCount() && a.offsets() == b.offsets() &&
           a.ids() == b.ids();
}

// Before a host edits nodes of a universe Arnold may be rendering (an interactive render's
// update): a render in progress is interrupted, and this waits until its threads have stopped,
// so no array they read is replaced under them. MtoA pauses the render before it calls a
// translator's Export or ExportMotion (MtoA 5.0 does: the status is AI_RENDER_STATUS_PAUSED
// there), and then this does nothing; it guards a host that does not, where replacing the
// curves' arrays mid-render would free memory the render threads still read. The host's own
// restart resumes the render after its update. True when it interrupted one.
inline bool interruptRenderForEdit(AtUniverse* universe) {
    AtRenderSession* session = AiUniverseGetRenderSession(universe);
    if (!session) return false;
    const AtRenderStatus status = AiRenderGetStatus(session);
    if (status != AI_RENDER_STATUS_RENDERING && status != AI_RENDER_STATUS_RESTARTING) return false;
    AiRenderInterrupt(session, AI_BLOCKING);
    return true;
}

// A strand Arnold's linear curves cannot take: fewer than two points (no segment), or a point
// that is not finite (NaN or infinite bounds break the acceleration structure Arnold builds).
inline bool unrenderableStrand(const StrandSet& s, std::size_t strand) {
    const std::size_t n = s.strandPointCount(strand);
    if (n < 2) return true;
    const Vec3f* p = s.strandPoints(strand);
    for (std::size_t k = 0; k < n; ++k) {
        if (!std::isfinite(p[k].x) || !std::isfinite(p[k].y) || !std::isfinite(p[k].z)) return true;
    }
    return false;
}

// The strands the curves node takes, as a plan over `strands` (the set an LOD `lod` was made
// on, when there is one): the LOD's kept strands (every strand without one) less the
// unrenderable ones (unrenderableStrand) and, where the feathers have a rachis (a Propagate
// node's shaftWidth: the rachis mesh renders them, setRachisMesh), their shafts; each kept
// strand's LOD width scale with it. Empty when that leaves out nothing beyond the LOD (the
// curves take what the LOD keeps, or every strand). `*dropped` gets how many unrenderable ones
// were left out.
inline std::optional<LodPlan> curvesPlan(const StrandSet& strands, const LodPlan* lod, std::size_t* dropped = nullptr) {
    const std::vector<float>* isShaft = hasRachis(strands.featherLayers()) ? strands.strandChannel(kRachisChannel) : nullptr;
    auto shaft = [&](std::size_t strand) { return isShaft && strand < isShaft->size() && (*isShaft)[strand] >= 0.5f; };
    std::size_t bad = 0, shafts = 0;
    LodPlan plan;
    const std::size_t count = lod ? lod->kept.size() : strands.strandCount();
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t strand = lod ? lod->kept[i] : i;
        if (strand < strands.strandCount() && !unrenderableStrand(strands, strand)) {
            shafts += shaft(strand) ? 1 : 0;
            continue;
        }
        ++bad;
    }
    if (dropped) *dropped = bad;
    if (bad == 0 && shafts == 0) return std::nullopt;
    plan.kept.reserve(count - bad - shafts);
    plan.widthScale.reserve(count - bad - shafts);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t strand = lod ? lod->kept[i] : i;
        if (strand >= strands.strandCount() || unrenderableStrand(strands, strand) || shaft(strand)) continue;
        plan.kept.push_back(strand);
        plan.widthScale.push_back(lod && i < lod->widthScale.size() ? lod->widthScale[i] : 1.0f);
    }
    if (lod) {
        plan.culled = lod->culled;
        plan.thinned = lod->thinned;
        plan.backfacing = lod->backfacing;
    }
    return plan;
}

// num_points, a `keys`-key points array (every key starts as this sample;
// setPointsKey fills the others), radius and a linear basis. Strand widths
// are diameters; Arnold wants radii, never negative or NaN (a negative Width or render width
// scale renders nothing rather than handing Arnold inverted bounds). The strands must be
// renderable (curvesPlan leaves out the ones that are not).
inline void setCurves(AtNode* curves, const ExportView& view, unsigned keys, float widthScale = 1.0f) {
    const StrandSet& s = *view.strands;
    const auto strandCount = static_cast<uint32_t>(view.strandCount());
    const auto pointCount = static_cast<uint32_t>(view.pointCount());

    AtArray* numPoints = AiArrayAllocate(strandCount, 1, AI_TYPE_UINT);
    auto* counts = static_cast<uint32_t*>(AiArrayMap(numPoints));
    for (uint32_t i = 0; i < strandCount; ++i) counts[i] = static_cast<uint32_t>(view.counts[i]);
    AiArrayUnmap(numPoints);

    AtArray* points = AiArrayAllocate(pointCount, static_cast<uint8_t>(keys), AI_TYPE_VECTOR);
    for (unsigned k = 0; k < keys; ++k) AiArraySetKey(points, static_cast<uint8_t>(k), s.positions().data());

    AtArray* radius = AiArrayAllocate(pointCount, 1, AI_TYPE_FLOAT);
    auto* r = static_cast<float*>(AiArrayMap(radius));
    const float scale = widthScale > 0.0f && std::isfinite(widthScale) ? widthScale : 0.0f;
    for (uint32_t i = 0; i < pointCount; ++i) {
        const float value = view.radius(i) * scale;
        r[i] = value > 0.0f && std::isfinite(value) ? value : 0.0f;
    }
    AiArrayUnmap(radius);

    AiNodeSetStr(curves, AtString("basis"), AtString("linear"));  // one radius per point, matches the viewport
    AiNodeSetArray(curves, AtString("num_points"), numPoints);
    AiNodeSetArray(curves, AtString("points"), points);
    AiNodeSetArray(curves, AtString("radius"), radius);
}

// Fills one key of the node's points array from `s`, or from the strands an
// LOD plan keeps of it (`plan` made on the same topology), without copying
// them into a StrandSet first. False when the topology differs from the
// array's (the caller keeps the existing key).
inline bool setPointsKey(AtNode* curves, const StrandSet& s, unsigned key, const LodPlan* plan = nullptr) {
    AtArray* points = AiNodeGetArray(curves, AtString("points"));
    if (!points || key >= AiArrayGetNumKeys(points)) return false;
    if (!plan) {
        if (AiArrayGetNumElements(points) != s.pointCount()) return false;
        return AiArraySetKey(points, static_cast<uint8_t>(key), s.positions().data());
    }
    std::size_t kept = 0;
    for (const std::size_t strand : plan->kept) {
        if (strand >= s.strandCount()) return false;
        kept += s.strandPointCount(strand);
    }
    if (AiArrayGetNumElements(points) != kept) return false;
    auto* out = static_cast<AtVector*>(AiArrayMapKey(points, static_cast<uint8_t>(key)));
    if (!out) return false;
    std::size_t at = 0;
    for (const std::size_t strand : plan->kept) {
        const std::size_t n = s.strandPointCount(strand);
        std::memcpy(out + at, s.strandPoints(strand), n * sizeof(AtVector));
        at += n;
    }
    AiArrayUnmap(points);
    return true;
}

// The curves modes GroomLab renders: ribbon and thick. Arnold's third mode,
// "oriented", needs `orientations`, which it does not support on the linear
// basis these curves use ("orientations for linear curves not supported"),
// so it is not offered; a stale request for it renders as ribbon.
inline const char* curvesMode(int mode) { return mode == 1 ? "thick" : "ribbon"; }
inline bool curvesModeKnown(int mode) { return mode == 0 || mode == 1; }

// Root UVs (`uvs`, when the scalp has them), strand_id (uniform UINT, low 32
// bits of the stable ID) and every strand channel as uniform FLOAT user data;
// hair_color (linear RGB, ExportView's color): varying, one value per point of
// the curves, when the strands carry the color nodes' per-point color (a tip blend,
// bands, a feather pattern), else uniform, one per strand. With FeatherColor's
// iridescence channel: iridescence (varying FLOAT) and root_normal (uniform
// RGB holding a direction, as user_data_rgb reads it: a feather's vane normal,
// else the scalp's at each root, `scalp` given, else each strand's first
// segment): what create_hair_shader's iridescence network reads. With
// RenderData's opacity channel: opacity (varying FLOAT), the curves not opaque
// when any point is below 1. With Wet's wetness channel: wetness (varying
// FLOAT). The view
// must describe the strands the curves' points came from (setCurves: the same
// set, after LOD), so the counts match.
//
// A re-export onto the same node (an interactive render's update) leaves nothing of the last one
// behind that would no longer match the strands: no uvs once the scalp has none, and, given
// `declared` (the user parameters the last call declared on this node; this call's on return),
// none of those this call does not set again (a strand channel the graph no longer writes,
// sized for the strands before).
inline void setStrandData(AtNode* curves, const ExportView& view, const ScalpMesh* scalp = nullptr,
                          std::vector<std::string>* declared = nullptr) {
    const StrandSet& s = *view.strands;
    const auto strandCount = static_cast<uint32_t>(view.strandCount());
    if (view.hasUVs()) {
        AtArray* uvs = AiArrayAllocate(strandCount, 1, AI_TYPE_VECTOR2);
        auto* uv = static_cast<AtVector2*>(AiArrayMap(uvs));
        for (uint32_t i = 0; i < strandCount; ++i) uv[i] = AtVector2(view.rootUVs[i][0], view.rootUVs[i][1]);
        AiArrayUnmap(uvs);
        AiNodeSetArray(curves, AtString("uvs"), uvs);
    } else if (const AtArray* uvs = AiNodeGetArray(curves, AtString("uvs")); uvs && AiArrayGetNumElements(uvs) > 0) {
        AiNodeResetParameter(curves, "uvs");
    }

    const AtNodeEntry* entry = AiNodeGetNodeEntry(curves);
    std::vector<std::string> setNow;
    // A user parameter declared before (a re-export) with another category or
    // type is undeclared first: hair_color turns varying or uniform as a tip
    // blend comes and goes.
    auto declare = [&](const AtString& name, const char* declaration, uint8_t category, uint8_t type) {
        if (AiNodeEntryLookUpParameter(entry, name)) return false;  // never shadow a real parameter
        setNow.emplace_back(name.c_str());
        if (const AtUserParamEntry* existing = AiNodeLookUpUserParameter(curves, name)) {
            const uint8_t declaredType = AiUserParamGetType(existing);
            const bool sameType =
                declaredType == type || (declaredType == AI_TYPE_ARRAY && AiUserParamGetArrayType(existing) == type);
            if (AiUserParamGetCategory(existing) == category && sameType) return true;
            AiNodeResetParameter(curves, name.c_str());  // removes a user parameter
        }
        if (AiNodeDeclare(curves, name, declaration)) return true;
        setNow.pop_back();
        return false;
    };
    const AtString idName("strand_id");
    if (declare(idName, "uniform UINT", AI_USERDEF_UNIFORM, AI_TYPE_UINT)) {
        AtArray* ids = AiArrayAllocate(strandCount, 1, AI_TYPE_UINT);
        auto* id = static_cast<uint32_t*>(AiArrayMap(ids));
        for (uint32_t i = 0; i < strandCount; ++i) id[i] = static_cast<uint32_t>(s.ids()[i]);
        AiArrayUnmap(ids);
        AiNodeSetArray(curves, idName, ids);
    }
    for (const std::string& channel : view.strandChannels) {
        const std::vector<float>* values = view.strandChannel(channel);
        const AtString name(channel.c_str());
        if (!values || !declare(name, "uniform FLOAT", AI_USERDEF_UNIFORM, AI_TYPE_FLOAT)) continue;
        AiNodeSetArray(curves, name, AiArrayConvert(strandCount, 1, AI_TYPE_FLOAT, values->data()));
    }
    const AtString colorName("hair_color");
    const auto pointCount = static_cast<uint32_t>(view.pointCount());
    const bool perPoint = view.pointColors.size() == pointCount && pointCount > 0;
    const std::vector<std::array<float, 3>>& source = perPoint ? view.pointColors : view.colors;
    const uint32_t count = perPoint ? pointCount : strandCount;
    if (source.size() == count && count > 0 &&
        declare(colorName, perPoint ? "varying RGB" : "uniform RGB", perPoint ? AI_USERDEF_VARYING : AI_USERDEF_UNIFORM,
                AI_TYPE_RGB)) {
        AtArray* colors = AiArrayAllocate(count, 1, AI_TYPE_RGB);
        auto* c = static_cast<AtRGB*>(AiArrayMap(colors));
        for (uint32_t i = 0; i < count; ++i) c[i] = AtRGB(source[i][0], source[i][1], source[i][2]);
        AiArrayUnmap(colors);
        AiNodeSetArray(curves, colorName, colors);
    } else if (AiNodeLookUpUserParameter(curves, colorName)) {
        AiNodeResetParameter(curves, colorName.c_str());  // a re-export without color: none left behind
    }
    // The structural color's share per point, and the normal its angle is taken from.
    const AtString iridescenceName("iridescence"), normalName("root_normal");
    const std::vector<float>* iridescence = view.pointChannel(kIridescenceChannel);
    if (iridescence && pointCount > 0 &&
        declare(iridescenceName, "varying FLOAT", AI_USERDEF_VARYING, AI_TYPE_FLOAT) &&
        declare(normalName, "uniform RGB", AI_USERDEF_UNIFORM, AI_TYPE_RGB)) {
        AiNodeSetArray(curves, iridescenceName, AiArrayConvert(pointCount, 1, AI_TYPE_FLOAT, iridescence->data()));
        AtArray* normals = AiArrayAllocate(strandCount, 1, AI_TYPE_RGB);
        auto* n = static_cast<AtRGB*>(AiArrayMap(normals));
        const VaneNormals vanes(s);
        for (uint32_t i = 0; i < strandCount; ++i) {
            const Vec3f up = iridescenceRootNormal(s, i, scalp, vanes);  // every host's (color_patterns.h)
            n[i] = AtRGB(up.x, up.y, up.z);
        }
        AiArrayUnmap(normals);
        AiNodeSetArray(curves, normalName, normals);
    } else {
        for (const AtString& name : {iridescenceName, normalName}) {
            if (!AiNodeEntryLookUpParameter(entry, name) && AiNodeLookUpUserParameter(curves, name)) AiNodeResetParameter(curves, name.c_str());
        }
    }
    // RenderData's opacity per point (varying FLOAT, what create_hair_shader's aiUserDataFloat
    // reads into the hair shader's opacity); the curves made not opaque when any point is below 1,
    // or Arnold would never run the shader's opacity. (HairColor's highlight channels are per strand:
    // uniform FLOAT user data, as every strand channel above.)
    const AtString opacityName("opacity");
    const std::vector<float>* opacity = view.pointChannel(kOpacityChannel);
    if (opacity && pointCount > 0 && declare(opacityName, "varying FLOAT", AI_USERDEF_VARYING, AI_TYPE_FLOAT)) {
        AiNodeSetArray(curves, opacityName, AiArrayConvert(pointCount, 1, AI_TYPE_FLOAT, opacity->data()));
        if (std::any_of(opacity->begin(), opacity->end(), [](float o) { return o < 1.0f; })) {
            AiNodeSetBool(curves, AtString("opaque"), false);
        }
    } else if (!AiNodeEntryLookUpParameter(entry, opacityName) && AiNodeLookUpUserParameter(curves, opacityName)) {
        AiNodeResetParameter(curves, opacityName.c_str());
    }
    // Wet's wetness per point (varying FLOAT: what create_hair_shader's wet network reads through
    // an aiUserDataFloat, hair_color.h wetShading), none left behind by a re-export without it.
    const AtString wetnessName(kWetnessChannel);
    const std::vector<float>* wetness = view.pointChannel(kWetnessChannel);
    if (wetness && pointCount > 0 && declare(wetnessName, "varying FLOAT", AI_USERDEF_VARYING, AI_TYPE_FLOAT)) {
        AiNodeSetArray(curves, wetnessName, AiArrayConvert(pointCount, 1, AI_TYPE_FLOAT, wetness->data()));
    } else if (!AiNodeEntryLookUpParameter(entry, wetnessName) && AiNodeLookUpUserParameter(curves, wetnessName)) {
        AiNodeResetParameter(curves, wetnessName.c_str());
    }
    // What the last call declared and this one did not set again: removed.
    if (declared) {
        for (const std::string& name : *declared) {
            if (std::find(setNow.begin(), setNow.end(), name) == setNow.end() &&
                AiNodeLookUpUserParameter(curves, AtString(name.c_str()))) {
                AiNodeResetParameter(curves, name.c_str());
            }
        }
        *declared = std::move(setNow);
    }
}

// The node's `shader` (a NODE[] on every shape, kick -info curves): one
// element, or empty for none.
inline void setShader(AtNode* node, AtNode* shader) {
    if (shader) {
        AiNodeSetArray(node, AtString("shader"), AiArrayConvert(1, 1, AI_TYPE_NODE, &shader));
    } else {
        AiNodeResetParameter(node, "shader");
    }
}

// Copies one parameter both nodes declare, whatever its type.
inline bool copyParameter(const AtNode* from, AtNode* to, const char* parameter) {
    const AtString name(parameter);
    const AtParamEntry* source = AiNodeEntryLookUpParameter(AiNodeGetNodeEntry(from), name);
    const AtParamEntry* target = AiNodeEntryLookUpParameter(AiNodeGetNodeEntry(to), name);
    if (!source || !target || AiParamGetType(source) != AiParamGetType(target)) return false;
    switch (AiParamGetType(source)) {
    case AI_TYPE_BYTE:
        AiNodeSetByte(to, name, AiNodeGetByte(from, name));
        return true;
    case AI_TYPE_BOOLEAN:
        AiNodeSetBool(to, name, AiNodeGetBool(from, name));
        return true;
    case AI_TYPE_INT:
        AiNodeSetInt(to, name, AiNodeGetInt(from, name));
        return true;
    case AI_TYPE_UINT:
        AiNodeSetUInt(to, name, AiNodeGetUInt(from, name));
        return true;
    case AI_TYPE_FLOAT:
        AiNodeSetFlt(to, name, AiNodeGetFlt(from, name));
        return true;
    case AI_TYPE_STRING:
        AiNodeSetStr(to, name, AiNodeGetStr(from, name));
        return true;
    case AI_TYPE_ENUM:
        AiNodeSetStr(to, name, AtString(AiEnumGetString(AiParamGetEnum(source), AiNodeGetInt(from, name))));
        return true;
    case AI_TYPE_ARRAY: {
        const AtArray* array = AiNodeGetArray(from, name);
        if (array && AiArrayGetNumElements(array) > 0) AiNodeSetArray(to, name, AiArrayCopy(array));
        return true;
    }
    default:
        return false;
    }
}

// The shading and visibility a procedural carries for its curves: shader,
// visibility, sidedness, matte, opaque, the shadow flags, light and shadow
// groups, trace sets, id and transform_type (kick -info procedural lists them;
// a shape procedural inherits them all).
inline void copyShapeParameters(const AtNode* from, AtNode* to) {
    for (const char* name : {"shader", "visibility", "sidedness", "matte", "opaque", "receive_shadows", "self_shadows",
                             "invert_normals", "ray_bias", "use_light_group", "light_group", "use_shadow_group",
                             "shadow_group", "trace_sets", "id", "transform_type"}) {
        copyParameter(from, to, name);
    }
}

// The render camera as an LOD camera in the groom's space (`groomToWorld`:
// the groom node's matrix), the way Arnold frames it: a perspective camera's
// `fov` spans the screen window's x range, the y range follows the image's
// aspect (options xres, yres and pixel_aspect_ratio, the way MtoA's film fit
// and offsets arrive), and an orthographic camera's screen window is in world
// units. Both hosts call this, so LOD sees the same view. False without a camera.
inline bool lodCameraFromArnold(const AtNode* camera, const AtNode* options, const AtMatrix& groomToWorld, LodCamera* out) {
    if (!camera || !out) return false;
    const AtMatrix toGroom = AiM4Mult(AiNodeGetMatrix(camera, AtString("matrix")), AiM4Invert(groomToWorld));
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out->matrix[r][c] = toGroom.data[r][c];
    }
    const AtNodeEntry* entry = AiNodeGetNodeEntry(camera);
    auto has = [&](const char* name) { return AiNodeEntryLookUpParameter(entry, AtString(name)) != nullptr; };
    // The screen window: the larger half-extent on each axis (an offset window is
    // widened rather than shifted, which keeps everything it can see).
    float halfX = 1.0f, halfY = 1.0f;
    if (has("screen_window_min") && has("screen_window_max")) {
        const AtArray* lo = AiNodeGetArray(camera, AtString("screen_window_min"));
        const AtArray* hi = AiNodeGetArray(camera, AtString("screen_window_max"));
        if (lo && hi && AiArrayGetNumElements(lo) > 0 && AiArrayGetNumElements(hi) > 0) {
            const AtVector2 a = AiArrayGetVec2(lo, 0), b = AiArrayGetVec2(hi, 0);
            halfX = std::max(std::abs(a.x), std::abs(b.x));
            halfY = std::max(std::abs(a.y), std::abs(b.y));
        }
    }
    float xres = 320.0f, yres = 240.0f, pixelAspect = 1.0f;
    if (options) {
        xres = static_cast<float>(std::max(AiNodeGetInt(options, AtString("xres")), 1));
        yres = static_cast<float>(std::max(AiNodeGetInt(options, AtString("yres")), 1));
        pixelAspect = AiNodeGetFlt(options, AtString("pixel_aspect_ratio"));
        if (!(pixelAspect > 0.0f)) pixelAspect = 1.0f;
    }
    const float deviceAspect = xres * pixelAspect / yres;
    float apertureAspect = 1.0f;
    if (has("aperture_aspect_ratio")) apertureAspect = AiNodeGetFlt(camera, AtString("aperture_aspect_ratio"));
    if (!(apertureAspect > 0.0f)) apertureAspect = 1.0f;

    out->orthographic = AiNodeIs(camera, AtString("ortho_camera"));
    if (out->orthographic) {
        out->orthoHalfWidth = halfX;
        out->orthoHalfHeight = halfY / deviceAspect;
        return true;
    }
    float fov = 54.43f;  // Maya's default camera
    if (has("fov")) {
        const AtArray* fovs = AiNodeGetArray(camera, AtString("fov"));
        if (fovs && AiArrayGetNumElements(fovs) > 0) fov = AiArrayGetFlt(fovs, 0);
        else fov = AiNodeGetFlt(camera, AtString("fov"));
    }
    if (!(fov > 0.0f) || fov >= 180.0f) fov = 90.0f;  // a lens LOD cannot describe: see the most it can
    const float tanHalf = pm::tan(0.5f * fov * AI_DTOR);  // (the same bits on every platform: portable_math.h)
    out->tanHalfFovX = tanHalf * halfX;
    out->tanHalfFovY = tanHalf * halfY / (deviceAspect * apertureAspect);
    return true;
}

inline AtMatrix instanceMatrix(const InstanceSet::Placed& p) {
    AtMatrix m;
    float values[16];
    p.matrix(values);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m.data[r][c] = values[r * 4 + c];
    }
    return m;
}

// What setInstancer placed, in the instancer's order: each instance's index in
// the InstanceSet and its ID, so later motion keys put the same instances in
// the same slots.
struct InstancerLayout {
    std::vector<std::size_t> order;
    std::vector<std::uint64_t> ids;
};

// Instances (the Instance node) as an Arnold instancer over `prototypes`
// (one node per InstanceSet prototype; null ones are skipped). Instances are
// visible to every ray even though the prototypes themselves are hidden.
// instance_matrix gets `keys` motion keys, every one this sample
// (setInstancerKey fills the others). Returns false when there is nothing to
// instance.
inline bool setInstancer(AtNode* instancer, const std::vector<AtNode*>& prototypes, const StrandSet& strands,
                         unsigned keys = 1, InstancerLayout* layout = nullptr) {
    if (!strands.instances()) return false;
    std::vector<uint32_t> slot(prototypes.size(), UINT32_MAX);
    std::vector<AtNode*> used;
    for (std::size_t p = 0; p < prototypes.size(); ++p) {
        if (!prototypes[p]) continue;
        slot[p] = static_cast<uint32_t>(used.size());
        used.push_back(prototypes[p]);
    }
    const auto placed = strands.instances()->resolve(strands);
    std::vector<AtMatrix> matrices;
    std::vector<uint32_t> indices;
    InstancerLayout placedLayout;
    matrices.reserve(placed.size());
    indices.reserve(placed.size());
    for (const auto& p : placed) {
        if (p.meshed || p.prototype >= slot.size() || slot[p.prototype] == UINT32_MAX) continue;  // (scales: setScaleMesh)
        matrices.push_back(instanceMatrix(p));
        indices.push_back(slot[p.prototype]);
        placedLayout.order.push_back(p.index);
        placedLayout.ids.push_back(p.id);
    }
    if (used.empty() || matrices.empty()) return false;
    keys = std::max(keys, 1u);
    AiNodeSetArray(instancer, AtString("nodes"), AiArrayConvert(static_cast<uint32_t>(used.size()), 1, AI_TYPE_NODE, used.data()));
    AtArray* matrixArray = AiArrayAllocate(static_cast<uint32_t>(matrices.size()), static_cast<uint8_t>(keys), AI_TYPE_MATRIX);
    for (unsigned k = 0; k < keys; ++k) AiArraySetKey(matrixArray, static_cast<uint8_t>(k), matrices.data());
    AiNodeSetArray(instancer, AtString("instance_matrix"), matrixArray);
    AiNodeSetArray(instancer, AtString("node_idxs"),
                   AiArrayConvert(static_cast<uint32_t>(indices.size()), 1, AI_TYPE_UINT, indices.data()));
    const std::vector<uint8_t> visible(indices.size(), AI_RAY_ALL);
    AiNodeSetArray(instancer, AtString("instance_visibility"),
                   AiArrayConvert(static_cast<uint32_t>(visible.size()), 1, AI_TYPE_BYTE, visible.data()));
    if (layout) *layout = std::move(placedLayout);
    return true;
}

// Fills motion key `key` of the instancer's instance_matrix from `strands` at
// that motion step: the instances of `layout`, found by their index in the
// InstanceSet (the Instance node makes the same list for the same strands).
// The strands may hold more (before render LOD) than the ones the instancer
// was made from. False, leaving the key as it was, when one of them is missing
// or is another instance (the strands changed between steps).
inline bool setInstancerKey(AtNode* instancer, const InstancerLayout& layout, const StrandSet& strands, unsigned key) {
    AtArray* array = AiNodeGetArray(instancer, AtString("instance_matrix"));
    if (!array || key >= AiArrayGetNumKeys(array) || AiArrayGetNumElements(array) != layout.order.size()) return false;
    if (!strands.instances()) return false;
    const auto placed = strands.instances()->resolve(strands);
    std::vector<const InstanceSet::Placed*> byIndex(strands.instances()->size(), nullptr);
    for (const auto& p : placed) byIndex[p.index] = &p;
    std::vector<AtMatrix> matrices(layout.order.size());
    for (std::size_t i = 0; i < layout.order.size(); ++i) {
        const std::size_t index = layout.order[i];
        if (index >= byIndex.size() || !byIndex[index] || byIndex[index]->id != layout.ids[i]) return false;
        matrices[i] = instanceMatrix(*byIndex[index]);
    }
    return AiArraySetKey(array, static_cast<uint8_t>(key), matrices.data());
}

// A prototype mesh (a built-in scale plate, scales.h) as a polymesh's geometry: its points,
// triangles, normals per point and uvs when it has them.
inline void setPrototypeMesh(AtNode* polymesh, const InstancePrototype& mesh) {
    const auto points = static_cast<uint32_t>(mesh.points.size());
    const auto corners = static_cast<uint32_t>(mesh.triangles.size());
    static_assert(sizeof(Vec3f) == sizeof(AtVector), "points convert as they are");
    AiNodeSetArray(polymesh, AtString("vlist"), AiArrayConvert(points, 1, AI_TYPE_VECTOR, mesh.points.data()));
    AiNodeSetArray(polymesh, AtString("nlist"), AiArrayConvert(points, 1, AI_TYPE_VECTOR, mesh.normals.data()));
    AiNodeSetArray(polymesh, AtString("vidxs"), AiArrayConvert(corners, 1, AI_TYPE_UINT, mesh.triangles.data()));
    AiNodeSetArray(polymesh, AtString("nidxs"), AiArrayConvert(corners, 1, AI_TYPE_UINT, mesh.triangles.data()));
    const std::vector<uint32_t> nsides(corners / 3, 3u);
    AiNodeSetArray(polymesh, AtString("nsides"), AiArrayConvert(static_cast<uint32_t>(nsides.size()), 1, AI_TYPE_UINT, nsides.data()));
    if (mesh.uvs.size() == mesh.points.size() && !mesh.uvs.empty()) {
        AiNodeSetArray(polymesh, AtString("uvlist"), AiArrayConvert(points, 1, AI_TYPE_VECTOR2, mesh.uvs.data()));
        AiNodeSetArray(polymesh, AtString("uvidxs"), AiArrayConvert(corners, 1, AI_TYPE_UINT, mesh.triangles.data()));
    }
    AiNodeSetBool(polymesh, AtString("smoothing"), true);
}

// The scales (scales.h) as one polymesh: every scale's bent mesh, its prototype's uvs, and for any
// surface shader the scale's own data as user data: per vertex `scaleColor` (its strand's
// HairColor, base to tip), per face `scaleRootColor` (at the root), `scaleId` (0-1, random per
// scale), `scaleUV` (the scalp's uv at the root: a position along the body) and `scaleVariant`
// (which of its node's prototypes). `shaders` are the polymesh's shaders and `slots` each
// scale's index into them (by its place in mesh.scales; empty: the first shader for all).
// vlist and nlist get `keys` motion keys, every one this mesh (setScaleMeshKey fills the others).
inline void setScaleMesh(AtNode* polymesh, const ScaleMesh& mesh, const std::vector<AtNode*>& shaders,
                         const std::vector<uint8_t>& slots, unsigned keys = 1) {
    const auto points = static_cast<uint32_t>(mesh.points.size());
    const auto corners = static_cast<uint32_t>(mesh.triangles.size());
    const uint32_t faces = corners / 3;
    keys = std::max(keys, 1u);
    for (const auto& [name, values] : {std::make_pair("vlist", &mesh.points), std::make_pair("nlist", &mesh.normals)}) {
        AtArray* array = AiArrayAllocate(points, static_cast<uint8_t>(keys), AI_TYPE_VECTOR);
        for (unsigned k = 0; k < keys; ++k) AiArraySetKey(array, static_cast<uint8_t>(k), values->data());
        AiNodeSetArray(polymesh, AtString(name), array);
    }
    AiNodeSetArray(polymesh, AtString("uvlist"), AiArrayConvert(points, 1, AI_TYPE_VECTOR2, mesh.uvs.data()));
    for (const char* indices : {"vidxs", "nidxs", "uvidxs"}) {
        AiNodeSetArray(polymesh, AtString(indices), AiArrayConvert(corners, 1, AI_TYPE_UINT, mesh.triangles.data()));
    }
    const std::vector<uint32_t> nsides(faces, 3u);
    AiNodeSetArray(polymesh, AtString("nsides"), AiArrayConvert(faces, 1, AI_TYPE_UINT, nsides.data()));
    AiNodeSetBool(polymesh, AtString("smoothing"), true);
    // Per face: whose scale it is.
    std::vector<float> ids(faces);
    std::vector<int> variants(faces);
    std::vector<std::array<float, 2>> uvs(faces);
    std::vector<std::array<float, 3>> rootColors(faces);
    std::vector<uint8_t> shaderIndices(slots.empty() ? 0 : faces);
    for (std::size_t k = 0; k < mesh.scales.size(); ++k) {
        const ScaleMesh::Scale& s = mesh.scales[k];
        for (uint32_t f = s.firstTriangle; f < s.firstTriangle + s.triangleCount && f < faces; ++f) {
            ids[f] = s.random;
            variants[f] = s.variant;
            uvs[f] = s.rootUv;
            rootColors[f] = s.color;
            if (!slots.empty()) shaderIndices[f] = k < slots.size() ? slots[k] : 0;
        }
    }
    AiNodeDeclare(polymesh, AtString("scaleColor"), "varying RGB");
    AiNodeSetArray(polymesh, AtString("scaleColor"), AiArrayConvert(points, 1, AI_TYPE_RGB, mesh.colors.data()));
    AiNodeDeclare(polymesh, AtString("scaleRootColor"), "uniform RGB");
    AiNodeSetArray(polymesh, AtString("scaleRootColor"), AiArrayConvert(faces, 1, AI_TYPE_RGB, rootColors.data()));
    AiNodeDeclare(polymesh, AtString("scaleId"), "uniform FLOAT");
    AiNodeSetArray(polymesh, AtString("scaleId"), AiArrayConvert(faces, 1, AI_TYPE_FLOAT, ids.data()));
    AiNodeDeclare(polymesh, AtString("scaleUV"), "uniform VECTOR2");
    AiNodeSetArray(polymesh, AtString("scaleUV"), AiArrayConvert(faces, 1, AI_TYPE_VECTOR2, uvs.data()));
    AiNodeDeclare(polymesh, AtString("scaleVariant"), "uniform INT");
    AiNodeSetArray(polymesh, AtString("scaleVariant"), AiArrayConvert(faces, 1, AI_TYPE_INT, variants.data()));
    std::vector<AtNode*> used;
    for (AtNode* shader : shaders) {
        if (shader) used.push_back(shader);
    }
    if (!used.empty()) {
        AiNodeSetArray(polymesh, AtString("shader"), AiArrayConvert(static_cast<uint32_t>(used.size()), 1, AI_TYPE_NODE, used.data()));
        if (!shaderIndices.empty() && used.size() == shaders.size() && used.size() > 1) {
            AiNodeSetArray(polymesh, AtString("shidxs"), AiArrayConvert(faces, 1, AI_TYPE_BYTE, shaderIndices.data()));
        }
    }
}

// Fills motion key `key` of the scales' vlist and nlist from `mesh`, the scales at that motion
// step (deformation blur: the skin moved, the strands laying the scales out with it). False,
// leaving the key as it was, when they are other scales (another count, other IDs) than `first`,
// the mesh setScaleMesh was given.
inline bool setScaleMeshKey(AtNode* polymesh, const ScaleMesh& first, const ScaleMesh& mesh, unsigned key) {
    if (mesh.points.size() != first.points.size() || mesh.scales.size() != first.scales.size()) return false;
    for (std::size_t k = 0; k < mesh.scales.size(); ++k) {
        if (mesh.scales[k].id != first.scales[k].id || mesh.scales[k].vertexCount != first.scales[k].vertexCount) return false;
    }
    bool ok = true;
    for (const auto& [name, values] : {std::make_pair("vlist", &mesh.points), std::make_pair("nlist", &mesh.normals)}) {
        AtArray* array = AiNodeGetArray(polymesh, AtString(name));
        ok = ok && array && key < AiArrayGetNumKeys(array) && AiArrayGetNumElements(array) == mesh.points.size() &&
             AiArraySetKey(array, static_cast<uint8_t>(key), values->data());
    }
    return ok;
}

// The feathers' rachis tubes (buildRachisMesh of the strands' feather layers) as a polymesh: its
// faces (quads and triangles, as the mesh has them), uvs (u around the tube, v root to tip), smooth
// normals, and per-vertex `rachisColor` (`colors`: rachisColors, each feather's shaft color).
// vlist gets `keys` motion keys, every one this mesh (setRachisMeshKey fills the others).
inline void setRachisMesh(AtNode* polymesh, const RachisMesh& mesh, const std::vector<std::array<float, 3>>& colors,
                          unsigned keys = 1) {
    const auto points = static_cast<uint32_t>(mesh.points.size());
    const auto corners = static_cast<uint32_t>(mesh.faceVertices.size());
    const auto faces = static_cast<uint32_t>(mesh.faceCounts.size());
    keys = std::max(keys, 1u);
    AtArray* vlist = AiArrayAllocate(points, static_cast<uint8_t>(keys), AI_TYPE_VECTOR);
    for (unsigned k = 0; k < keys; ++k) AiArraySetKey(vlist, static_cast<uint8_t>(k), mesh.points.data());
    AiNodeSetArray(polymesh, AtString("vlist"), vlist);
    std::vector<uint32_t> nsides(mesh.faceCounts.begin(), mesh.faceCounts.end());
    std::vector<uint32_t> vidxs(mesh.faceVertices.begin(), mesh.faceVertices.end());
    std::vector<uint32_t> uvidxs(mesh.faceUVs.begin(), mesh.faceUVs.end());
    AiNodeSetArray(polymesh, AtString("nsides"), AiArrayConvert(faces, 1, AI_TYPE_UINT, nsides.data()));
    AiNodeSetArray(polymesh, AtString("vidxs"), AiArrayConvert(corners, 1, AI_TYPE_UINT, vidxs.data()));
    std::vector<std::array<float, 2>> uvs(mesh.u.size());
    for (std::size_t i = 0; i < uvs.size(); ++i) uvs[i] = {mesh.u[i], i < mesh.v.size() ? mesh.v[i] : 0.0f};
    AiNodeSetArray(polymesh, AtString("uvlist"), AiArrayConvert(static_cast<uint32_t>(uvs.size()), 1, AI_TYPE_VECTOR2, uvs.data()));
    AiNodeSetArray(polymesh, AtString("uvidxs"), AiArrayConvert(corners, 1, AI_TYPE_UINT, uvidxs.data()));
    AiNodeSetBool(polymesh, AtString("smoothing"), true);
    if (colors.size() == mesh.points.size()) {
        AiNodeDeclare(polymesh, AtString("rachisColor"), "varying RGB");
        AiNodeSetArray(polymesh, AtString("rachisColor"), AiArrayConvert(points, 1, AI_TYPE_RGB, colors.data()));
    }
}

// Fills motion key `key` of the rachis's vlist from `mesh`, the rachis at that motion step; false,
// leaving the key as it was, when it is another mesh (other point count) than `first`'s.
inline bool setRachisMeshKey(AtNode* polymesh, const RachisMesh& first, const RachisMesh& mesh, unsigned key) {
    if (mesh.points.size() != first.points.size() || mesh.faceVertices != first.faceVertices) return false;
    AtArray* array = AiNodeGetArray(polymesh, AtString("vlist"));
    return array && key < AiArrayGetNumKeys(array) && AiArrayGetNumElements(array) == mesh.points.size() &&
           AiArraySetKey(array, static_cast<uint8_t>(key), mesh.points.data());
}

// What a rachis renders with: a standard_surface whose base color reads its `rachisColor`
// (`userData`, a user_data_rgb node), a little glossy, as a feather's shaft is.
inline void setDefaultRachisShader(AtNode* surface, AtNode* userData) {
    AiNodeSetStr(userData, AtString("attribute"), AtString("rachisColor"));
    AiNodeSetRGB(userData, AtString("default"), 0.5f, 0.5f, 0.5f);
    AiNodeSetFlt(surface, AtString("base"), 1.0f);
    AiNodeSetFlt(surface, AtString("specular"), 0.5f);
    AiNodeSetFlt(surface, AtString("specular_roughness"), 0.3f);
    AiNodeLink(userData, "base_color", surface);
}

// What the built-in plates render with when no material is named: a standard_surface whose base
// color reads the scales' `scaleColor` (`userData`, a user_data_rgb node).
inline void setDefaultScaleShader(AtNode* surface, AtNode* userData) {
    AiNodeSetStr(userData, AtString("attribute"), AtString("scaleColor"));
    AiNodeSetRGB(userData, AtString("default"), 0.5f, 0.5f, 0.5f);
    AiNodeSetFlt(surface, AtString("base"), 1.0f);
    AiNodeSetFlt(surface, AtString("specular"), 0.6f);
    AiNodeSetFlt(surface, AtString("specular_roughness"), 0.35f);
    AiNodeLink(userData, "base_color", surface);
}

}  // namespace groomlab::arnold
