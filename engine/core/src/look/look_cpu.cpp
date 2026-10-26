// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_cpu.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>

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

void runOnCpuEngine(const LookRun& run, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out, bool widths) {
    // A thread's own engine, kept: its kernels' scratch reused, the strands it ran last let go of
    // once the next come (setBase).
    thread_local std::unique_ptr<LookEngine> engine;
    if (!engine) engine = makeCpuLookEngine();
    engine->setBase(in, scalp);
    LookTargets targets;
    targets.baseAsOutput = true;
    bool moves = false;
    for (const StageParams& s : run.params.stages) moves = moves || s.stage.kind != StageKind::Width;
    if (moves) targets.positions = out.mutablePositions().data();
    if (widths) {
        std::vector<float>& w = out.mutableWidths();
        w.resize(out.pointCount());
        targets.widths = w.data();
    }
    engine->run(run, false, targets);
    engine->dropTargets();
}

}  // namespace groomlab::gpu
