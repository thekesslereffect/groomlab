// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_cpu.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>

#include "groomlab/graph.h"
#include "groomlab/parallel.h"

namespace groomlab::gpu {

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

namespace {

// The stage `kind` of an operator's run: its params, its inputs and its mapped params' values on `in`
// (the strands `which` alone: a view's), or the node's error where the engine cannot take them.
LookRun stageRun(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs,
                 const std::vector<std::uint32_t>* which) {
    const ParamSet& params = reader.params();
    LookRun run;
    StageParams& s = run.params.stages.emplace_back();
    s.stage.kind = kind;
    if (params.has("seed")) s.seed = static_cast<std::uint64_t>(params.getInt("seed"));
    std::string why;
    if (!readStageParams(kind, reader, &s, &why)) throw GraphError(std::string(stageName(kind)) + ": " + why);
    if (which && kind == StageKind::Curl && s.flag[2] != 0) {
        throw GraphError("curl: by clump it reads its clumps' other strands (in full)");
    }
    run.fields.stages.resize(1);
    run.inputs.push_back(std::move(inputs));
    const std::array<std::string, 2> mapped = mappedParams(kind, params);
    const bool coil = kind == StageKind::Curl && params.getString("shape") == "coil";
    if (coil) {
        // A coil's radius by the strands' widths at each point (times coilWidths), as they carry them
        // (none: 1 everywhere); by the whole set's points (a view reads them there).
        FieldValues& values = run.fields.stages[0].field[0];
        values.mode = FieldValues::kPerPoint;
        values.values = in->widths().size() == in->pointCount() ? in->widths() : std::vector<float>(in->pointCount(), 1.0f);
        values.fixed = true;
        values.widths = true;
    }
    for (int i = 0; i < 2; ++i) {
        if (mapped[i].empty() || !mappedParam(params, mapped[i]) || (coil && i == 0)) continue;
        if (kind == StageKind::Direction && reader.directions) continue;  // (its three per strand: StageInputs::directions)
        FieldValues& values = run.fields.stages[0].field[i];
        const ScalarField field(params, mapped[i], scalp);
        if (which) {
            readFieldValues(field, *in, *which, mappedPerPoint(kind, i), &values);
        } else {
            readFieldValues(field, *in, mappedPerPoint(kind, i), &values);
        }
    }
    return run;
}

// `run` on `engine` into `targets`, the engine's errors the node's.
void runOn(LookEngine& engine, StageKind kind, const LookRun& run, const LookTargets& targets) {
    try {
        engine.run(run, false, targets);
    } catch (const GraphError&) {
        engine.dropTargets();
        engine.letGoOfBase();
        throw;
    } catch (const std::exception& e) {
        engine.dropTargets();
        engine.letGoOfBase();
        throw GraphError(std::string(stageName(kind)) + ": " + e.what());
    }
    engine.dropTargets();
    engine.letGoOfBase();
}

}  // namespace

StrandsPtr runStage(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    return runStage(kind, StageParamReader(params, ""), in, scalp, std::move(inputs));
}

StrandsPtr runStage(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    if (!in || in->empty()) return in;
    auto out = std::make_shared<StrandSet>(*in);
    runStageInto(kind, reader, in, scalp, *out, std::move(inputs));
    return out;
}

void runStageOnStrands(StageKind kind, const ParamSet& params, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs) {
    runStageOnStrands(kind, StageParamReader(params, ""), in, which, scalp, out, std::move(inputs));
}

void runStageOnStrands(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs) {
    if (!in || which.empty()) return;
    const LookRun run = stageRun(kind, reader, in, scalp, std::move(inputs), &which);
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
    runOn(engine, kind, run, targets);
}

void runStageInto(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out, StageInputs inputs) {
    runStageInto(kind, StageParamReader(params, ""), in, scalp, out, std::move(inputs));
}

void runStageInto(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out,
                  StageInputs inputs) {
    if (!in || in->empty()) return;
    const LookRun run = stageRun(kind, reader, in, scalp, std::move(inputs), nullptr);
    // (Bend's, Direction's, Transform's and Displace's root frames: the engine's own, LookEngine::putStageInputs.)
    LookEngine& engine = cpuEngineOfThread();
    engine.setBase(in, scalp);
    LookTargets targets;
    targets.baseAsOutput = true;
    if (kind == StageKind::Width) {
        std::vector<float>& w = out.mutableWidths();
        w.resize(out.pointCount());
        targets.widths = w.data();
    } else {
        // (A stage moving points writes every one, as on the GPU: no copy of the base's first.)
        targets.positions = out.overwritePositions().data();
    }
    runOn(engine, kind, run, targets);
}

}  // namespace groomlab::gpu
