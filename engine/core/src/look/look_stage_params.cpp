// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_stage_params.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <unordered_map>
#include <variant>

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/guide_set.h"
#include "groomlab/map_ref.h"
#include "groomlab/parallel.h"
#include "groomlab/parts.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"

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
        case StageKind::Interpolate:
        case StageKind::Length:
        case StageKind::Clump:
        case StageKind::Noise:
        case StageKind::Width:
        case StageKind::Part:
        case StageKind::Gravity:
        case StageKind::Flyaway:
        case StageKind::Collide:
        case StageKind::Wind:
        case StageKind::Bend:
        case StageKind::Direction:
        case StageKind::Transform:
        case StageKind::Displace:
        case StageKind::Twist:
        case StageKind::Smooth:
        case StageKind::BlendNeighbors:
        case StageKind::Tangle:
        case StageKind::Flow:
        case StageKind::RestoreLength:
        case StageKind::MeshCut: return true;
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
        case StageKind::Interpolate:
            if (params.getString("take") != "shape") {
                *why = r.name() + " takes the guides' length alone: the CPU's";
                return false;
            }
            if (params.getBool("fillTubes")) {
                *why = r.name() + " fills the guides' tubes (fillTubes): the CPU's";
                return false;
            }
            s->value[0] = static_cast<float>(number("limitDistance"));
            s->flag[0] = static_cast<int>(r.samples);
            s->flag[1] = params.getBool("preserveLength") ? 1 : 0;
            s->ramp = r.ramp("falloff");
            return true;
        case StageKind::Clump: {
            // Its strength and volumize mapped; the rest numbers and choices (clumpShape).
            if (!mappable("strength", &s->value[0]) || !mappable("volumize", &s->value[1])) return false;
            clumpShape(params, [&](const char* name) { return number(name); }, s);
            s->ramp = r.ramp("profile");
            s->ramp2 = r.ramp("volumizeProfile");
            return true;
        }
        case StageKind::Noise: {
            if (!mappable("amplitude", &s->value[0]) || !mappable("fraction", &s->value[2])) return false;
            s->value[1] = static_cast<float>(number("frequency"));
            s->flag[0] = static_cast<int>(std::floor(number("octaves")));
            s->flag[1] = params.getString("space") == "world" ? 1 : 0;
            s->flag[2] = params.getString("mode") == "kinky" ? 1 : 0;
            s->ramp = r.ramp("profile");  // a plugged curve too
            s->tabled = true;
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
        case StageKind::Collide: {
            if (params.getString("method") != "field") {
                *why = r.name() + " asks the mesh at every point (method exact: the CPU's)";
                return false;
            }
            const auto offset = static_cast<float>(number("offset"));
            const auto maxDepth = static_cast<float>(params.getDouble("maxDepth"));
            const auto softness = static_cast<float>(params.getDouble("softness"));
            s->value[0] = offset;
            s->value[1] = maxDepth;
            s->value[2] = std::max(offset + softness, maxDepth) * 1.001f + 1e-6f;  // CollideOp's search radius
            s->flag[0] = static_cast<int>(params.getInt("iterations"));
            s->value2[0] = softness;
            s->value2[1] = static_cast<float>(params.getDouble("smoothing"));
            return true;
        }
        case StageKind::Wind: {
            // At the reader's time, as WindOp works it out: how far the gusts travelled along the
            // direction and the flutter's phase (in double, as the CPU's, then rounded to floats as
            // it rounds them).
            if (!mappable("strength", &s->value[0])) return false;
            const auto frequency = static_cast<float>(number("frequency"));
            const double speed = number("speed");
            s->value[1] = static_cast<float>(number("gust"));
            s->value[2] = frequency;
            s->value[3] = static_cast<float>(number("turbulence"));
            const Vec3f direction = params.getVec3("direction");
            const Vec3f dir = direction.length() < 1e-6f ? Vec3f{} : direction.normalized();
            const WindPhase phase = windPhase(dir, frequency, speed, r.time, r.framesPerSecond, params.getBool("loop"),
                                              static_cast<double>(params.getInt("loopStart")), static_cast<double>(params.getInt("loopEnd")));
            auto bits = [](int* to, float v) { std::memcpy(to, &v, sizeof(float)); };  // the kernel reads them as floats
            if (!(direction.length() < 1e-6f)) {
                s->value2[0] = dir.x;
                s->value2[1] = dir.y;
                s->value2[2] = dir.z;
                if (phase.looped) {
                    bits(&s->flag[0], phase.angle);
                    bits(&s->flag[1], phase.turnsPer);
                    s->flag[3] = 1;
                    bits(&s->flag2[0], phase.across1.x);
                    bits(&s->flag2[1], phase.across1.y);
                    bits(&s->flag2[2], phase.across1.z);
                    bits(&s->flag2[3], phase.radius);
                } else {
                    bits(&s->flag[0], phase.shift.x);
                    bits(&s->flag[1], phase.shift.y);
                    bits(&s->flag[2], phase.shift.z);
                }
            }
            s->value2[3] = phase.flutterPhase;
            s->ramp = r.ramp("profile");
            s->tabled = true;
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
        // ---- The shapers reading their neighbours (their neighbours their operators' own) ----
        case StageKind::Smooth: {
            if (!mappable("strength", &s->value[0])) return false;
            const float scale = std::clamp(static_cast<float>(number("scale")), 0.0f, 1.0f);
            s->value[1] = scale;
            // extra_ops.cpp smoothCells: four cells to the deviation, 8 to 128.
            s->flag[0] = static_cast<int>(std::clamp(std::ceil(4.0f / std::max(scale, 1e-3f)), 8.0f, 128.0f));
            s->ramp = r.ramp("profile");
            s->tabled = true;
            return true;
        }
        case StageKind::BlendNeighbors:
            if (!mappable("strength", &s->value[0])) return false;
            s->flag[0] = params.getBool("preserveLength") ? 1 : 0;
            s->ramp = r.ramp("profile");
            s->tabled = true;
            return true;
        case StageKind::Tangle:
            if (!mappable("strength", &s->value[0])) return false;
            s->value[1] = static_cast<float>(number("start"));
            s->value[2] = static_cast<float>(number("span"));
            s->value[3] = static_cast<float>(number("reach"));
            return true;
        case StageKind::RestoreLength: {
            // Its mode; a reference a stage of the run makes (flag[1]): the preview's to say.
            const std::string mode = params.getString("mode");
            s->flag[0] = mode == "scale" ? 1 : mode == "even" ? 2 : 0;
            return true;
        }
        case StageKind::MeshCut:
            s->value[0] = static_cast<float>(number("offset"));
            s->flag[0] = params.getString("keep") == "inside" ? 1 : 0;
            return true;
        case StageKind::Flow:
            // FlowOp's numbers: the lift in degrees (its multiplier), its jitter in radians.
            if (!mappable("lift", &s->value[0])) return false;
            s->value[1] = static_cast<float>(number("bend"));
            s->value[2] = static_cast<float>(number("rollRandom")) * 0.0174532925f;
            return true;
        default: *why = r.name() + ": not a stage read here"; return false;
    }
}

std::shared_ptr<ClumpInputs> clumpInputs(std::shared_ptr<const std::vector<std::uint32_t>> assigned, std::vector<Vec3f> curves,
                                         std::vector<Vec3f> normals, const std::vector<StrandId>& ids, std::uint64_t seed) {
    auto made = std::make_shared<ClumpInputs>();
    made->assigned = std::move(assigned);
    made->centers = ids.size();
    made->curves = std::make_shared<const std::vector<Vec3f>>(std::move(curves));
    made->normals = std::make_shared<const std::vector<Vec3f>>(std::move(normals));
    made->variation = clumpVariation(ids, seed);
    return made;
}

std::shared_ptr<const std::vector<float>> clumpVariation(const std::vector<StrandId>& ids, std::uint64_t seed) {
    auto variation = std::make_shared<std::vector<float>>(ids.size() * 4, 0.0f);
    for (std::size_t c = 0; c < ids.size(); ++c) {
        float* v = variation->data() + c * 4;
        v[0] = unitFloat(hashCombine(ids[c], seed ^ 0x57E4Aull));
        v[1] = unitFloat(hashCombine(ids[c], seed ^ 0x2AD1Bull));
        v[2] = unitFloat(hashCombine(ids[c], seed ^ 0xF1A7));
    }
    return variation;
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

std::shared_ptr<TwistInputs> twistInputs(const StrandSet& in, const GuideSet* axesIn, const std::string& channel, bool nearestByKernel,
                                         std::string* why) {
    const std::size_t strands = in.strandCount();
    auto words = std::make_shared<std::vector<std::uint32_t>>(strands * 4, 0u);
    auto made = std::make_shared<TwistInputs>();
    auto rootBits = [&](std::size_t s) {
        if (in.strandPointCount(s) == 0) return;
        std::memcpy(words->data() + s * 4 + 1, &in.strandPoints(s)[0], 12);
    };
    if (axesIn && !axesIn->empty()) {
        // TwistOp: each guide of two points or more resampled by arc length, and each strand's
        // nearest by its root.
        auto axes = std::make_shared<std::vector<float>>();
        std::vector<Vec3f> roots;
        for (const Guide& g : axesIn->guides()) {
            if (g.points.size() < 2) continue;
            const std::vector<Vec3f> pts = resampleByArcLength(g.points.data(), g.points.size(), kTwistAxisSamples);
            roots.push_back(pts.front());
            for (const Vec3f& q : pts) axes->insert(axes->end(), {q.x, q.y, q.z});
        }
        made->count = roots.size();
        for (std::size_t a = 0; a < roots.size(); ++a) {
            axes->push_back(arcLength(reinterpret_cast<const Vec3f*>(axes->data() + a * kTwistAxisSamples * 3), kTwistAxisSamples));
        }
        if (nearestByKernel) {
            std::fill(words->begin(), words->end(), 0xffffffffu);  // (no root's bits: TwistCS finds each nearest)
        } else if (!roots.empty()) {
            const PointIndex index(roots);
            parallelFor(0, strands, [&](std::size_t s) {
                (*words)[s * 4] = in.strandPointCount(s) == 0 ? 0xffffffffu : static_cast<std::uint32_t>(index.nearest(in.strandPoints(s)[0]));
                rootBits(s);
            }, 1024);
        }
        if (axes->empty()) axes->push_back(0.0f);
        made->axes = std::move(axes);
    } else {
        // Around each clump's middle: its core strands' mean (by the channel, any key).
        const std::vector<float>* groups = in.strandChannel(channel);
        if (!groups) {
            *why = "no axes, and no '" + channel + "' channel to find the clumps (the CPU's)";
            return nullptr;
        }
        // Each key's slot in the order the keys first come (small whole numbers, as a Clump numbers
        // its clumps, by a table; others by a map).
        const PreviewCore core(in);
        std::vector<long long> keys(strands);
        std::vector<std::uint8_t> cores(strands);
        parallelFor(0, strands, [&](std::size_t s) {
            keys[s] = std::llround((*groups)[s]);
            cores[s] = core.core(s) ? 1 : 0;
        }, 4096);
        long long lowest = 0, highest = -1;
        if (strands > 0) {
            const auto [lo, hi] = std::minmax_element(keys.begin(), keys.end());
            lowest = *lo;
            highest = *hi;
        }
        const bool tabled = strands > 0 && lowest >= 0 && highest < static_cast<long long>(4 * strands + 1024);
        std::vector<std::uint32_t> table(tabled ? static_cast<std::size_t>(highest) + 1 : 0, 0xffffffffu);
        std::map<long long, std::uint32_t> slot;
        std::vector<std::uint32_t> slotOf(strands), counts;
        for (std::size_t s = 0; s < strands; ++s) {
            std::uint32_t at = 0;
            if (tabled) {
                std::uint32_t& t = table[static_cast<std::size_t>(keys[s])];
                if (t == 0xffffffffu) {
                    t = static_cast<std::uint32_t>(counts.size());
                    counts.push_back(0);
                }
                at = t;
            } else {
                auto it = slot.find(keys[s]);
                if (it == slot.end()) {
                    it = slot.emplace(keys[s], static_cast<std::uint32_t>(counts.size())).first;
                    counts.push_back(0);
                }
                at = it->second;
            }
            slotOf[s] = at;
            counts[at] += cores[s];
        }
        // The clumps' core strands (count + 1 offsets, then each clump's in strand order).
        const std::size_t clumps = counts.size();
        auto list = std::make_shared<std::vector<std::uint32_t>>(clumps + 1, 0u);
        for (std::size_t c = 0; c < clumps; ++c) (*list)[c + 1] = (*list)[c] + counts[c];
        list->resize(clumps + 1 + (*list)[clumps]);
        std::vector<std::uint32_t> next(list->begin(), list->begin() + static_cast<std::ptrdiff_t>(clumps));
        for (std::size_t s = 0; s < strands; ++s) {
            if (cores[s]) (*list)[clumps + 1 + next[slotOf[s]]++] = static_cast<std::uint32_t>(s);
        }
        parallelFor(0, strands, [&](std::size_t s) {
            (*words)[s * 4] = counts[slotOf[s]] ? slotOf[s] : 0xffffffffu;  // no core strand: it does not turn
        }, 4096);
        made->members = std::move(list);
        made->count = clumps;
    }
    made->strands = std::move(words);
    return made;
}

std::shared_ptr<TangleInputs> tangleInputs(const StrandSet& in, const TangleState& state) {
    const std::size_t count = in.strandCount();
    auto words = std::make_shared<std::vector<std::uint32_t>>(count * 4);
    parallelFor(0, count, [&](std::size_t s) {
        std::uint32_t* w = words->data() + s * 4;
        const std::uint32_t p = state.partners[s];
        w[0] = p < count ? p : kNoTanglePartner;
        w[1] = w[2] = w[3] = 0u;
        if (w[0] == kNoTanglePartner) return;
        // TangleOp: a pair that picked each other meets in the middle over the span of its lower
        // ID; a strand whose partner picked another moves all the way onto it.
        const bool mutual = state.partners[p] == s;
        const float draw = mutual && in.ids()[p] < in.ids()[s] ? state.draws[p] : state.draws[s];
        const float share = mutual ? 0.5f : 1.0f;
        std::memcpy(&w[1], &draw, 4);
        std::memcpy(&w[2], &share, 4);
    }, 4096);
    auto made = std::make_shared<TangleInputs>();
    made->strands = std::move(words);
    return made;
}

std::shared_ptr<CollideInputs> collideInputs(const std::vector<MeshPtr>& meshes, std::uint32_t colliders,
                                             const std::vector<DistanceFieldPtr>& fields, float offset, float softness, std::string* why) {
    if (meshes.size() > 2 || fields.size() != meshes.size()) {
        *why = "more surfaces than the kernel reads (the CPU's)";
        return nullptr;
    }
    for (const DistanceFieldPtr& field : fields) {
        if (!field) {
            *why = "a surface has no field the GPU can read (deformed: the CPU's)";
            return nullptr;
        }
    }
    auto made = std::make_shared<CollideInputs>();
    made->colliders = colliders;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        // cellSizeFor rounds the offset's log2 to the nearest whole number: the offsets of a cell
        // are under sqrt 2 times it (any past the largest cell it takes are the largest's), and
        // those of a reach are within it.
        const MeshDistanceField& field = *fields[m];
        float upTo = field.reach();
        if (MeshDistanceField::cellSizeFor(*meshes[m], std::numeric_limits<float>::max()) > field.cellSize()) {
            upTo = std::min(upTo, field.cellSize() * 1.4143f + softness);
        }
        MeshDistanceField::Tables tables = field.tables(std::max(offset + softness, upTo), field.reach());
        if (!tables) {
            *why = "a surface has no field the GPU can read (too big for its grid: the CPU's)";
            return nullptr;
        }
        made->tables.push_back(std::move(tables));
        const MeshPtr& mesh = meshes[m];
        Vec3f lo = mesh->points().empty() ? Vec3f{} : mesh->points().front(), hi = lo;
        for (const Vec3f& q : mesh->points()) {
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        made->boxes.push_back(lo);
        made->boxes.push_back(hi);
    }
    return made;
}

std::shared_ptr<RestoreInputs> restoreInputs(const StrandSet& in, const StrandSet* refStrands, const GuideSet* refGuides, float* longest,
                                             bool* found) {
    // The reference curves by ID (RestoreLengthOp: strands, or guides), each at the strand's point
    // count as the node resamples it.
    struct Ref {
        const Vec3f* points;
        std::size_t count;
    };
    std::unordered_map<StrandId, Ref> reference;
    if (refStrands) {
        reference.reserve(refStrands->strandCount());
        for (std::size_t s = 0; s < refStrands->strandCount(); ++s) {
            if (refStrands->strandPointCount(s) >= 2) reference[refStrands->ids()[s]] = {refStrands->strandPoints(s), refStrands->strandPointCount(s)};
        }
    } else if (refGuides) {
        for (const Guide& g : refGuides->guides()) {
            if (g.points.size() >= 2) reference[g.id] = {g.points.data(), g.points.size()};
        }
    }
    if (found) *found = !reference.empty();
    auto lengths = std::make_shared<std::vector<float>>(in.pointCount(), 0.0f);
    auto firsts = std::make_shared<std::vector<float>>(in.strandCount() * 4, 0.0f);
    std::vector<float> reaches(in.strandCount(), 0.0f);
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        const std::size_t n = in.strandPointCount(s), begin = in.strandBegin(s);
        if (n == 0) return;
        (*lengths)[begin] = -1.0f;
        if (n < 2) return;
        const auto it = reference.find(in.ids()[s]);
        if (it == reference.end()) return;
        const Ref& ref = it->second;
        thread_local std::vector<Vec3f> sampled;
        const Vec3f* pts = ref.points;
        if (ref.count != n) {
            sampled.resize(n);
            resampleByArcLengthInto(ref.points, ref.count, sampled.data(), n);
            pts = sampled.data();
        }
        const float whole = arcLength(ref.points, ref.count);
        (*lengths)[begin] = whole;
        reaches[s] = whole;
        for (std::size_t i = 1; i < n; ++i) (*lengths)[begin + i] = (pts[i] - pts[i - 1]).length();
        const Vec3f first = pts[1] - pts[0];
        float* f = firsts->data() + s * 4;
        f[0] = first.x;
        f[1] = first.y;
        f[2] = first.z;
    }, 1024);
    if (longest) {
        *longest = 0.0f;
        for (const float r : reaches) *longest = std::max(*longest, r);
    }
    auto made = std::make_shared<RestoreInputs>();
    made->lengths = std::move(lengths);
    made->firsts = std::move(firsts);
    return made;
}

std::shared_ptr<MeshCutInputs> meshCutInputs(const MeshPtr& mesh) {
    auto made = std::make_shared<MeshCutInputs>();
    if (mesh && mesh->triangleCount() > 0) made->tree = mesh->rayTree();
    return made;
}

std::shared_ptr<PartInputs> partInputs(const StrandSet& in, const PartLines* lines, const ScalpMesh* scalp, const std::string& only,
                                       float within) {
    auto sides = std::make_shared<std::vector<float>>(in.strandCount() * 4, 0.0f);
    const bool open = lines && !lines->empty() && scalp;
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        float* to = sides->data() + s * 4;
        to[3] = -1.0f;
        const StrandRoot& root = in.roots()[s];
        if (!open || in.strandPointCount(s) < 2 || !scalp->contains(root)) return;
        const PartLines::Side side = lines->sideOf(scalp->restPositionOr(root, in.strandPoints(s)[0]), only);
        if (side.side == 0 || side.distance >= within) return;
        const Vec3f away = lines->awayFrom(side, scalp->normal(root));
        to[0] = away.x;
        to[1] = away.y;
        to[2] = away.z;
        to[3] = side.distance;
    }, 1024);
    auto made = std::make_shared<PartInputs>();
    made->sides = std::move(sides);
    return made;
}

}  // namespace groomlab::gpu
