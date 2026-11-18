// SPDX-License-Identifier: Apache-2.0
#include <string>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "internal.h"

namespace groomlab {

void registerBuiltinOperators(OperatorRegistry& registry) {
    // Each line of builtin_operators.inc: its register<Type>Operator, in the type's source file.
#define GROOMLAB_OPERATOR(Type) register##Type##Operator(registry);
#include "groomlab/builtin_operators.inc"
#undef GROOMLAB_OPERATOR
}

}  // namespace groomlab
