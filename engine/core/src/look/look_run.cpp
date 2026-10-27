// SPDX-License-Identifier: Apache-2.0
#include "groomlab/look/look_run.h"

namespace groomlab::gpu {

const char* stageName(StageKind kind) {
    static const char* const names[] = {
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) name,
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
    };
    const auto k = static_cast<std::size_t>(kind);
    return k < static_cast<std::size_t>(kStageKinds) ? names[k] : "?";
}

std::size_t LookChain::indexOf(NodeId id) const {
    for (std::size_t k = 0; k < stages.size(); ++k) {
        if (stages[k].node == id) return k;
    }
    return kNoStage;
}

NodeId LookChain::node(StageKind kind) const {
    for (const LookStage& s : stages) {
        if (s.kind == kind) return s.node;
    }
    return 0;
}

const StageParams* LookParams::find(StageKind kind) const {
    for (const StageParams& s : stages) {
        if (s.stage.kind == kind) return &s;
    }
    return nullptr;
}
StageParams* LookParams::find(StageKind kind) {
    for (StageParams& s : stages) {
        if (s.stage.kind == kind) return &s;
    }
    return nullptr;
}

bool LookFields::any() const {
    for (const StageFields& s : stages) {
        if (s.field[0].mode || s.field[1].mode || s.affect.mode) return true;
    }
    return false;
}

bool runsWidth(const LookParams& params) {
    for (const StageParams& s : params.stages) {
        if (s.stage.kind == StageKind::Width || (s.branch && runsWidth(s.branch->params))) return true;
    }
    return false;
}

}  // namespace groomlab::gpu
