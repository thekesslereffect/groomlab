// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

namespace groomlab {

class OperatorRegistry;

// Built-in operator type names (the "type" field in graph JSON): op::kScalp = "Scalp" and so on,
// one for each line of builtin_operators.inc (which says what each takes and gives).
namespace op {
#define GROOMLAB_OPERATOR(Type) constexpr const char* k##Type = #Type;
#include "groomlab/builtin_operators.inc"
#undef GROOMLAB_OPERATOR
}  // namespace op

// The most earlier frames a Jiggle node reads (its `history` is clamped to 1..this).
constexpr int kMaxScalpHistory = 48;

// What a node type does, in a line (empty for a type it does not know): every host's param panel
// and add-node search show it (operatorSpecsJson's "description"). The registry's NodeInfo
// (node_catalog.h), given where the type registers.
std::string operatorDescription(const std::string& type);

// Registers every built-in operator (builtin_operators.inc, in its order).
// OperatorRegistry::instance() calls this on first use.
void registerBuiltinOperators(OperatorRegistry& registry);

// Work the operators keep between evaluations, counted over the process (tests check what the
// memos save): how many times a Scatter relaxed its roots (randomness below 1; a Scatter keeps
// its relaxed roots on the rest pose, so a deforming scalp does not relax them again), and how
// many times an Interpolate bound its strands to guides (each strand's guides and weights, kept
// while the roots, guide roots, parts and binding params stay; a brush dab only re-blends).
std::size_t scatterRelaxCount();
// Relaxation iterations run so far in this process (tests: a settled layout stops early).
std::size_t scatterRelaxIterationCount();
std::size_t interpolateBindCount();
// How many strands Interpolate bindings searched guides for: when only some guide roots moved (a
// brush dab re-roots the guides it touches), the strands no moved guide reaches keep theirs.
std::size_t interpolateStrandBindCount();
// Tests: Interpolate binds as first written (every strand's guides from the kd-tree) instead of by
// its faster paths, which must give the same strands bit for bit.
void setInterpolateReferencePaths(bool reference);

}  // namespace groomlab
