// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_stage_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <variant>

#include "groomlab/field.h"
#include "groomlab/map_ref.h"
#include "groomlab/parallel.h"

namespace groomlab::gpu {

bool StageParamReader::constantVector(const std::string& param, Vec3f* out) const {
    try {
        const VectorField field(params_, param, nullptr);
        *out = field.constant();
        return field.isConstant();
    } catch (const std::exception&) {
        return false;
    }
}

bool StageParamReader::mappable(const std::string& param, float* value, std::string*) const {
    *value = mappedParam(params_, param) ? 1.0f : static_cast<float>(number(param));
    return true;
}

bool readsStageParams(StageKind kind) {
    switch (kind) {
        case StageKind::Length:
        case StageKind::Width:
        case StageKind::Part:
        case StageKind::Gravity:
        case StageKind::Flyaway:
        case StageKind::Bend:
        case StageKind::Direction:
        case StageKind::Transform:
        case StageKind::Displace:
        case StageKind::Twist: return true;
        default: return false;
    }
}

bool readStageParams(StageKind kind, const StageParamReader& r, StageParams* s, std::string* why) {
    const ParamSet& params = r.params();
    auto number = [&](const char* name) { return r.number(name); };
    auto mappable = [&](const char* name, float* value) { return r.mappable(name, value, why); };
    switch (kind) {
        case StageKind::Length: {
            if (!mappable("keep", &s->value[0]) || !mappable("fraction", &s->value[1])) return false;
            const auto a = static_cast<float>(number("trimMin")), b = static_cast<float>(number("trimMax"));
            s->value[2] = std::min(a, b);
            s->value[3] = std::max(a, b);
            return true;
        }
        case StageKind::Width: {
            if (!mappable("width", &s->value[0])) return false;
            s->value[1] = static_cast<float>(number("randomness"));
            s->ramp = r.ramp("profile");
            return true;
        }
        case StageKind::Part: {
            if (!mappable("strength", &s->value[0])) return false;
            s->value[1] = static_cast<float>(number("width"));
            s->ramp = r.ramp("profile");
            s->tabled = true;
            return true;
        }
        case StageKind::Gravity: {
            // The direction a world vector (as every strand takes it, normalized as the CPU
            // does); one per strand (an expression, a painted color set) is the CPU's.
            bool constant = false;
            Vec3f down;
            try {
                const VectorField direction(params, "direction", nullptr);
                constant = direction.isConstant();
                down = direction.constant();
            } catch (const std::exception&) {
                constant = false;
            }
            if (!constant || !params.getString("directionColorSet").empty()) {
                *why = r.name() + " takes its direction per strand (the CPU's)";
                return false;
            }
            if (!mappable("strength", &s->value[0])) return false;
            s->value[1] = static_cast<float>(number("stiffness"));
            if (!(down.length() < 1e-6f)) {
                const Vec3f unit = down.normalized();
                s->value2[0] = unit.x;
                s->value2[1] = unit.y;
                s->value2[2] = unit.z;
            }
            s->ramp = r.ramp("stiffnessProfile");  // by the arc's share (its keys)
            return true;
        }
        case StageKind::Flyaway: {
            if (!mappable("stray", &s->value[0]) || !mappable("fraction", &s->value[1])) return false;
            s->value[2] = static_cast<float>(number("frequency"));
            s->value[3] = static_cast<float>(number("start"));
            s->value2[0] = static_cast<float>(number("standOut"));
            s->value2[1] = static_cast<float>(number("lengthen"));
            s->flag[0] = 1;  // the stray's noise: one octave
            return true;
        }
        // ---- The shapers working on each strand alone ----
        case StageKind::Bend:
        case StageKind::Direction:
        case StageKind::Displace: {
            // The direction one world vector (as every strand takes it); one per strand (an
            // expression) is the CPU's.
            Vec3f direction;
            if (!r.constantVector("direction", &direction)) {
                *why = r.name() + " takes its direction per strand (the CPU's)";
                return false;
            }
            if (kind == StageKind::Bend) {
                if (!mappable("angle", &s->value[0])) return false;
                s->value[1] = static_cast<float>(number("start"));
                s->value[2] = static_cast<float>(number("end"));
                s->value[3] = static_cast<float>(number("randomness"));
                const std::string along = params.getString("along");
                s->flag[0] = along == "normal" ? 0 : along == "direction" ? 2 : 1;
                s->value2[0] = direction.x;
                s->value2[1] = direction.y;
                s->value2[2] = direction.z;
            } else if (kind == StageKind::Direction) {
                // Its mapped params' fields: the first two mapped, a third the CPU's.
                const std::vector<std::string> fields = directionMapped(params);
                if (fields.size() > 2) {
                    *why = r.name() + " maps its inclination, roll and strength all three (the CPU's)";
                    return false;
                }
                const char* const names[3] = {"inclination", "roll", "strength"};
                for (int i = 0; i < 3; ++i) {
                    if (!mappable(names[i], &s->value[i])) return false;
                    const auto at = std::find(fields.begin(), fields.end(), names[i]);
                    s->flag[1 + i] = at == fields.end() ? 2 : static_cast<int>(at - fields.begin());
                }
                s->value[3] = static_cast<float>(number("inclinationRandom"));
                s->value2[0] = direction.x;
                s->value2[1] = direction.y;
                s->value2[2] = direction.z;
                s->value2[3] = static_cast<float>(number("rollRandom"));
                s->flag[0] = params.getString("along") == "tangent" ? 1 : 0;
            } else {
                if (!mappable("distance", &s->value[0])) return false;
                s->flag[0] = params.getString("along") != "direction" ? 1 : 0;
                const Vec3f unit = direction.length() > 1e-6f ? direction.normalized() : Vec3f{};  // (DisplaceOp's `unit`)
                s->value2[0] = unit.x;
                s->value2[1] = unit.y;
                s->value2[2] = unit.z;
                s->ramp = r.ramp("profile");
                s->tabled = true;
            }
            return true;
        }
        case StageKind::Transform: {
            if (!mappable("scale", &s->value[0])) return false;
            const Vec3f translate = r.vector("translate"), rotate = r.vector("rotate"), pivot = r.vector("pivot");
            const Vec3f translateRandom = r.vector("translateRandom"), rotateRandom = r.vector("rotateRandom");
            const float scaleRandom = static_cast<float>(number("scaleRandom"));
            s->value[1] = scaleRandom;
            const Vec3f zero;
            s->flag[0] = params.getString("space") == "world" ? 1 : 0;
            s->flag[2] = translateRandom != zero ? 1 : 0;
            s->flag[3] = rotateRandom != zero ? 1 : 0;
            // Nothing to do (TransformOp's `identity`: its input handed on as it came).
            s->flag[1] = translate == zero && rotate == zero && !s->flag[2] && !s->flag[3] && scaleRandom <= 0.0f &&
                                 !mappedParam(params, "scale") && s->value[0] == 1.0f
                             ? 1
                             : 0;
            const Vec3f* rows[4] = {&rotate, &pivot, &translateRandom, &rotateRandom};
            for (int i = 0; i < 4; ++i) {
                s->more[i * 4] = rows[i]->x;
                s->more[i * 4 + 1] = rows[i]->y;
                s->more[i * 4 + 2] = rows[i]->z;
            }
            s->value2[0] = translate.x;
            s->value2[1] = translate.y;
            s->value2[2] = translate.z;
            return true;
        }
        case StageKind::Twist: {
            if (!mappable("turns", &s->value[0])) return false;
            s->ramp = r.ramp("profile");
            s->tabled = true;
            return true;
        }
        default: *why = r.name() + ": not a stage read here"; return false;
    }
}

bool ownMap(const ParamSet& params, const std::string& name) {
    return params.has(name) && std::get_if<MapRef>(&params.get(name)) != nullptr;
}

bool mappedParam(const ParamSet& params, const std::string& name) { return ownMap(params, name) || params.field(name); }

std::vector<std::string> directionMapped(const ParamSet& params) {
    std::vector<std::string> names;
    for (const char* name : {"inclination", "roll", "strength"}) {
        if (mappedParam(params, name)) names.push_back(name);
    }
    return names;
}

std::array<std::string, 2> mappedParams(StageKind kind, const ParamSet& params) {
    static const char* const kNames[kStageKinds][2] = {
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) {mapped0, mapped1},
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
    };
    if (kind == StageKind::Direction) {
        const std::vector<std::string> mapped = directionMapped(params);
        return {mapped.size() > 0 ? mapped[0] : std::string(), mapped.size() > 1 ? mapped[1] : std::string()};
    }
    return {kNames[static_cast<int>(kind)][0], kNames[static_cast<int>(kind)][1]};
}

bool mappedPerPoint(StageKind kind, int i) {
    return i == 0 && (kind == StageKind::Clump || kind == StageKind::Noise || kind == StageKind::Curl || kind == StageKind::Width ||
                      kind == StageKind::Blend || kind == StageKind::Displace);
}

bool movesPointByPoint(StageKind kind) {
    return kind == StageKind::Clump || kind == StageKind::Noise || kind == StageKind::Curl || kind == StageKind::Displace;
}

namespace {

// transform_ops.cpp uvTangent: the direction U grows in across face `face` of a scalp with UVs
// (zero where the UVs give none).
Vec3f scalpUvTangent(const ScalpMesh& scalp, std::int32_t face) {
    const auto& tri = scalp.triangles()[static_cast<std::size_t>(face)];
    const auto& uv = scalp.cornerUVs();
    const std::size_t c = static_cast<std::size_t>(face) * 3;
    if (c + 2 >= uv.size()) return {};
    const Vec3f a = scalp.points()[static_cast<std::size_t>(tri[0])];
    const Vec3f e1 = scalp.points()[static_cast<std::size_t>(tri[1])] - a;
    const Vec3f e2 = scalp.points()[static_cast<std::size_t>(tri[2])] - a;
    const float du1 = uv[c + 1][0] - uv[c][0], dv1 = uv[c + 1][1] - uv[c][1];
    const float du2 = uv[c + 2][0] - uv[c][0], dv2 = uv[c + 2][1] - uv[c][1];
    const float det = du1 * dv2 - du2 * dv1;
    if (!(std::fabs(det) > 1e-12f)) return {};
    return (e1 * dv2 - e2 * dv1) / det;
}

}  // namespace

void writeRootFrames(const StrandSet& in, const ScalpMesh* scalp, float* frames) {
    const bool uvs = scalp && scalp->hasUVs();
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        float* to = frames + s * 8;
        const StrandRoot& root = in.roots()[s];
        if (!scalp || !scalp->contains(root)) {
            std::fill_n(to, 8, 0.0f);
            return;
        }
        const Vec3f n = scalp->normal(root);
        const Vec3f u = uvs ? scalpUvTangent(*scalp, root.face) : Vec3f{};
        to[0] = n.x;
        to[1] = n.y;
        to[2] = n.z;
        to[3] = 1.0f;
        to[4] = u.x;
        to[5] = u.y;
        to[6] = u.z;
        to[7] = 0.0f;
    }, 4096);
}

std::shared_ptr<const RootInputs> rootInputs(const StrandSet& in, const ScalpMesh* scalp) {
    auto frames = std::make_shared<std::vector<float>>(in.strandCount() * 8);
    writeRootFrames(in, scalp, frames->data());
    auto made = std::make_shared<RootInputs>();
    made->frames = std::move(frames);
    return made;
}

}  // namespace groomlab::gpu
