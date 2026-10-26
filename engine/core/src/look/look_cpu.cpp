// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_cpu.h"

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

void runOnCpuEngine(const LookRun& run, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out, bool widths, const std::string& tail) {
    // A thread's own engine, kept: its kernels' scratch reused, the strands it ran last let go of
    // once the next come (setBase).
    thread_local std::unique_ptr<LookEngine> engine;
    if (!engine) engine = makeCpuLookEngine();
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
    if (!tail.empty()) {
        std::vector<float> written = engine->readTail(0);
        if (written.size() != out.pointCount()) throw std::runtime_error("CPU look engine: the tail channel not written");
        if (std::vector<float>* had = out.mutablePointChannel(tail)) {
            *had = std::move(written);
        } else {
            out.addPointChannel(tail, 0.0f) = std::move(written);
        }
    }
}

StrandsPtr runStage(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    return runStage(kind, StageParamReader(params, ""), in, scalp, std::move(inputs));
}

StrandsPtr runStage(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs) {
    if (!in || in->empty()) return nullptr;
    auto out = std::make_shared<StrandSet>(*in);
    return runStageInto(kind, reader, in, scalp, *out, std::move(inputs)) ? out : nullptr;
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
        FieldValues& values = run.fields.stages[0].field[i];
        readFieldValues(ScalarField(params, mapped[i], scalp), *in, mappedPerPoint(kind, i), &values);
        // Read at each point after the ones before it moved: the CPU's.
        if (values.readsPositions && values.mode == FieldValues::kPerPoint && movesPointByPoint(kind)) return false;
    }
    // (Bend's, Direction's, Transform's and Displace's root frames: the engine's own, LookEngine::putStageInputs.)
    std::string tail;
    if (kind == StageKind::Tie) {
        tail = params.getString("tailChannel");
        run.tailsOut = !tail.empty();
    }
    runOnCpuEngine(run, in, scalp, out, kind == StageKind::Width, tail);
    return true;
}

}  // namespace groomlab::gpu
