// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the engine's backends share: one dispatch's constants as the kernels lay them out
// (look.slang's cbuffer Look, and the same rows as GLSL's std140 uniform block).

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>

#include "groomlab/look/look_engine.h"

namespace groomlab::gpu {

// One dispatch's constant buffer, 16-byte rows (look.slang cbuffer Look).
struct alignas(16) StageConstants {
    std::uint32_t strandCount, uniformCount, pointCount, dataBlock;
    float value[4];
    std::uint32_t flag[4];
    std::uint32_t octaveSeeds[16];  // 8 x (lo, hi)
    std::uint32_t ramp[4];          // key count, offset, straight, table count
    std::uint32_t table[4];         // table offset, the lines' normals block
    std::uint32_t field[4];         // field 0 mode, offset; field 1 mode, offset
    std::uint32_t aux[4];
    std::uint32_t dispatch[4];      // the groups in a row of the dispatch
    float value2[4];                // more numbers (StageParams::value2; a field's program: min, max)
    std::uint32_t flag2[4];         // more switches (StageParams::flag2; Interpolate: write the influence)
    std::uint32_t ramp2[4];         // a second profile (Clump's volumizeProfile), as `ramp`
    std::uint32_t table2[4];        // its table's offset
};
static_assert(sizeof(StageConstants) % 16 == 0, "cbuffer rows");
// Each dispatch's slot in one buffer: OpenGL binds ranges of it, at its uniform offset alignment
// (256 at most on every driver).
constexpr std::size_t kConstantSlotBytes = 256;
static_assert(sizeof(StageConstants) <= kConstantSlotBytes, "a slot holds a dispatch's constants");

// Timestamps a run takes at most: before the first node, after each node (and its centers), the
// frames and the copy, and after each point's strand made.
constexpr std::size_t kMaxStamps = 2 * LookEngine::kMaxStages + 5;
static_assert(sizeof(StageConstants) == kConstantSlotBytes, "look.slang's cbuffer Look: 16 rows");

// The kernels a run of `run` may dispatch, a bit each (LookEngine::Kernel): never fewer than it does
// (each of its stages' kernels, a Clump's centers' branch's; the lines', gather's, point
// texture's and fields' always).
using KernelSet = std::bitset<static_cast<std::size_t>(LookEngine::Kernel::Count)>;
KernelSet kernelsOf(const LookRun& run);
// The kernels' names ("Collide, Clearance"), for a host to say what is compiling.
std::string kernelNames(KernelSet kernels);
// "Collide" for Kernel::Collide.
const char* kernelName(LookEngine::Kernel kernel);

}  // namespace groomlab::gpu
