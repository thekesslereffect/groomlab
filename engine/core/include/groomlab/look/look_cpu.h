// SPDX-License-Identifier: Apache-2.0
#pragma once

// The core's operators on the CPU engine (look_engine.h makeCpuLookEngine: look.slang's kernels as
// C++, one source with the GPU preview's): an operator builds its own stage (its params, its mapped
// params' values on the strands it takes) and runs it here.

#include "groomlab/field.h"
#include "groomlab/look/look_engine.h"
#include "groomlab/look/look_stage_params.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::gpu {

// Whether the operators run their stages on the CPU engine (the default; GROOMLAB_CPU_ENGINE=0: their
// own C++, for comparisons).
bool cpuEngineOn();
// Tests and benchmarks: on or off from now on, whatever GROOMLAB_CPU_ENGINE says.
void setCpuEngineOn(bool on);

// One mapped param's values on `in` (the strands its node takes), as `field` reads them there: per
// point where it varies along the strands and the operator reads it so (`pointsAllowed`), else per
// strand (at the root, ScalarField::at). Binds the field to `in` (ScalarField::validate: throws
// GraphError as the operator would).
void readFieldValues(const ScalarField& field, const StrandSet& in, bool pointsAllowed, FieldValues* out);
// The same for the strands `which` of `in` alone, as a set of them in that order holds them (read on
// `in` itself: an expression of `index` reads each strand's place there).
void readFieldValues(const ScalarField& field, const StrandSet& in, const std::vector<std::uint32_t>& which, bool pointsAllowed,
                     FieldValues* out);

// `run` (its stages from `in`) on the calling thread's CPU engine (made once, kept), into `out` (`in`
// copied): a run moving points writes every one into out's own (StrandSet::overwritePositions), and
// with `widths` the widths a Width makes (out's widths, one a point). Throws std::runtime_error as
// the engine does.
void runOnCpuEngine(const LookRun& run, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out, bool widths = false);

// An operator's own stage of `kind` (readStageParams' kinds, look_stage_params.h) on `in`: its
// params and its mapped params' values on `in` read here, its inputs `inputs` (the root frames the
// engine's own), the result `in` copied with the points (a Width: the widths) the kernel makes. Null
// where the kernel does not run it so (the operator's C++ then).
StrandsPtr runStage(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs = {});
// The same with the params as `reader` reads them (an operator's own at its time: Wind).
StrandsPtr runStage(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StageInputs inputs = {});
// The same into `out` (`in` copied, its points its own: the operator's, carrying what else it adds):
// false, `out` untouched, where the kernel does not run it so.
bool runStageInto(StageKind kind, const ParamSet& params, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out,
                  StageInputs inputs = {});
bool runStageInto(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const ScalpMesh* scalp, StrandSet& out,
                  StageInputs inputs = {});
// The stage on the strands `which` of `in` alone (an incremental evaluation's changed strands, of a
// stage whose strands each read only themselves: its fields strand-local, its inputs none or no
// strand's), on a view of them (LookEngine::setBaseView), their points (a Width: their widths)
// written into `out` in place (`in`'s layout: the last result's copy, the rest left as they are).
// False, `out` untouched, where the kernel does not run it so.
bool runStageOnStrands(StageKind kind, const ParamSet& params, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs = {});
bool runStageOnStrands(StageKind kind, const StageParamReader& reader, const StrandsPtr& in, const std::vector<std::uint32_t>& which,
                       const ScalpMesh* scalp, StrandSet& out, StageInputs inputs = {});

}  // namespace groomlab::gpu
