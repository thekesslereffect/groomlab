// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_cpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "groomlab/parallel.h"

namespace groomlab::gpu {

namespace {
std::atomic<int> engineOn{-1};  // -1: as GROOMLAB_CPU_ENGINE says (read once)
}  // namespace

bool cpuEngineOn() {
    int on = engineOn.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* v = std::getenv("GROOMLAB_CPU_ENGINE");
        on = v && std::strcmp(v, "0") == 0 ? 0 : 1;
        int unset = -1;
        engineOn.compare_exchange_strong(unset, on);
    }
    return on != 0;
}

void setCpuEngineOn(bool on) { engineOn.store(on ? 1 : 0, std::memory_order_relaxed); }

void readFieldValues(const ScalarField& field, const StrandSet& in, bool pointsAllowed, FieldValues* out) {
    field.validate(in);  // binds channels and expressions to the set; throws GraphError as the operator would
    out->readsPositions = field.readsPositions();
    if (pointsAllowed && field.perPoint()) {
        out->mode = FieldValues::kPerPoint;
        out->values.assign(in.pointCount(), 0.0f);
        parallelFor(0, in.strandCount(), [&](std::size_t s) {
            const std::size_t begin = in.strandBegin(s), n = in.strandPointCount(s);
            for (std::size_t i = 0; i < n; ++i) out->values[begin + i] = field.atPoint(in, s, i);
        }, 1024);
    } else {
        out->mode = FieldValues::kPerStrand;
        out->values.assign(in.strandCount(), 0.0f);
        parallelFor(0, in.strandCount(), [&](std::size_t s) { out->values[s] = field.at(in, s); }, 1024);
    }
}

void readFieldValues(const ScalarField& field, const StrandSet& in, const std::vector<std::uint32_t>& which, bool pointsAllowed,
                     FieldValues* out) {
    field.validate(in);
    out->readsPositions = field.readsPositions();
    if (pointsAllowed && field.perPoint()) {
        out->mode = FieldValues::kPerPoint;  // (by the whole set's points: a view reads them there)
        out->values.assign(in.pointCount(), 0.0f);
        parallelFor(0, which.size(), [&](std::size_t k) {
            const std::size_t begin = in.strandBegin(which[k]);
            for (std::size_t i = 0; i < in.strandPointCount(which[k]); ++i) out->values[begin + i] = field.atPoint(in, which[k], i);
        }, 64);
    } else {
        out->mode = FieldValues::kPerStrand;
        out->values.assign(which.size(), 0.0f);
        parallelFor(0, which.size(), [&](std::size_t k) { out->values[k] = field.at(in, which[k]); }, 256);
    }
}

namespace {
// A thread's own engine, kept: its kernels' scratch reused, the strands it ran last let go of once
// the next come (setBase).
LookEngine& cpuEngineOfThread() {
    thread_local std::unique_ptr<LookEngine> engine;
    if (!engine) engine = makeCpuLookEngine();
    return *engine;
}
}  // namespace

void runOnCpuEngine(const LookRun& run, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out, bool widths) {
    LookEngine* engine = &cpuEngineOfThread();
    engine->setBase(in, scalp);
    LookTargets targets;
    targets.baseAsOutput = true;
    bool moves = false;
    for (const StageParams& s : run.params.stages) moves = moves || s.stage.kind != StageKind::Width;
    // (A stage moving points writes every one, as on the GPU: no copy of the base's first.)
    if (moves) targets.positions = out.overwritePositions().data();
    if (widths) {
        std::vector<float>& w = out.mutableWidths();
        w.resize(out.pointCount());
        targets.widths = w.data();
    }
    engine->run(run, false, targets);
    engine->dropTargets();
    engine->letGoOfBase();
}

StrandsPtr runStage(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    return runStage(kind, StageParamReader(params, ""), in, scalp, std::move(inputs));
}

StrandsPtr runStage(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    if (!in || in->empty()) return nullptr;
    auto out = std::make_shared<StrandSet>(*in);
    return runStageInto(kind, reader, in, scalp, *out, std::move(inputs)) ? out : nullptr;
}

bool runStageOnStrands(StageKind kind, const ParamSet& params, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs) {
    return runStageOnStrands(kind, StageParamReader(params, ""), in, which, scalp, out, std::move(inputs));
}

bool runStageOnStrands(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs) {
    if (!in || which.empty()) return false;
    switch (kind) {  // (stages whose inputs, if any, are no strand's: their root frames the engine's own)
        case StageKind::Noise:
        case StageKind::Width:
        case StageKind::Length:
        case StageKind::Bend:
        case StageKind::Direction:
        case StageKind::Transform:
        case StageKind::Displace:
        case StageKind::Gravity:
        case StageKind::Flyaway:
        case StageKind::Wind:
        case StageKind::Smooth:
        case StageKind::Collide:
        case StageKind::MeshCut:
        case StageKind::Clump:  // (their centers the caller's, for these strands)
        case StageKind::Part:  // (their sides the caller's, for these strands)
        case StageKind::Flow:  // (their roots traced the caller's)
        case StageKind::Twist:  // (around axes: their axes the caller's, for these strands)
        case StageKind::Jiggle:  // (their lags the caller's, for these strands)
        case StageKind::Interpolate:  // (their binding the caller's, for these strands)
        case StageKind::RestoreLength:  // (their reference the caller's, for these strands)
        case StageKind::Blend:  // (look b handed in: StageInputs::blendWith)
        case StageKind::Curl: break;  // (alone: by clump it reads its clumps)
        default: return false;
    }
    const ParamSet& params = reader.params();
    LookRun run;
    StageParams& s = run.params.stages.emplace_back();
    s.stage.kind = kind;
    if (params.has("seed")) s.seed = static_cast<std::uint64_t>(params.getInt("seed"));
    std::string why;
    if (!readStageParams(kind, reader, &s, &why)) return false;
    run.fields.stages.resize(1);
    run.inputs.push_back(std::move(inputs));
    const std::array<std::string, 2> mapped = mappedParams(kind, params);
    const bool coil = kind == StageKind::Curl && params.getString("shape") == "coil";
    if (kind == StageKind::Curl && (s.flag[2] != 0 || (coil && in->widths().size() != in->pointCount()))) return false;
    if (coil) {  // (runStageInto's: by the whole set's points, a view reads them there)
        FieldValues& values = run.fields.stages[0].field[0];
        values.mode = FieldValues::kPerPoint;
        values.values = in->widths();
        values.fixed = true;
        values.widths = true;
    }
    for (int i = 0; i < 2; ++i) {
        if (mapped[i].empty() || !mappedParam(params, mapped[i]) || (coil && i == 0)) continue;
        if (kind == StageKind::Direction && reader.directions) continue;  // (its three per strand: StageInputs::directions)
        FieldValues& values = run.fields.stages[0].field[i];
        readFieldValues(ScalarField(params, mapped[i], scalp), *in, which, mappedPerPoint(kind, i), &values);
    }
    // The thread's engine (runOnCpuEngine's) on a view of these strands, its result written into
    // `out` in place (their points; a Width: their widths).
    LookEngine& engine = cpuEngineOfThread();
    engine.setBaseView(in, scalp, std::make_shared<const std::vector<std::uint32_t>>(which));
    LookTargets targets;
    targets.baseAsOutput = true;
    if (kind == StageKind::Width) {
        std::vector<float>& w = out.mutableWidths();
        w.resize(out.pointCount(), 1.0f);
        targets.widths = w.data();
    } else {
        targets.positions = out.mutablePositions().data();
    }
    engine.run(run, false, targets);
    engine.dropTargets();
    engine.letGoOfBase();
    return true;
}

bool runStageInto(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out,
                  StageInputs inputs) {
    return runStageInto(kind, StageParamReader(params, ""), in, scalp, out, std::move(inputs));
}

bool runStageInto(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out,
                  StageInputs inputs) {
    if (!in || in->empty()) return false;
    const ParamSet& params = reader.params();
    LookRun run;
    StageParams& s = run.params.stages.emplace_back();
    s.stage.kind = kind;
    if (params.has("seed")) s.seed = static_cast<std::uint64_t>(params.getInt("seed"));
    std::string why;
    if (!readStageParams(kind, reader, &s, &why)) return false;
    run.fields.stages.resize(1);
    run.inputs.push_back(std::move(inputs));
    const std::array<std::string, 2> mapped = mappedParams(kind, params);
    const bool coil = kind == StageKind::Curl && params.getString("shape") == "coil";
    if (coil) {
        // A coil's radius by the strands' widths at each point (times coilWidths), as they carry them.
        if (in->widths().size() != in->pointCount()) return false;
        FieldValues& values = run.fields.stages[0].field[0];
        values.mode = FieldValues::kPerPoint;
        values.values = in->widths();
        values.fixed = true;
        values.widths = true;
    }
    for (int i = 0; i < 2; ++i) {
        if (mapped[i].empty() || !mappedParam(params, mapped[i]) || (coil && i == 0)) continue;
        if (kind == StageKind::Direction && reader.directions) continue;  // (its three per strand: StageInputs::directions)
        FieldValues& values = run.fields.stages[0].field[i];
        readFieldValues(ScalarField(params, mapped[i], scalp), *in, mappedPerPoint(kind, i), &values);
    }
    // (Bend's, Direction's, Transform's and Displace's root frames: the engine's own, LookEngine::putStageInputs.)
    runOnCpuEngine(run, in, scalp, out, kind == StageKind::Width);
    return true;
}

}  // namespace groomlab::gpu
