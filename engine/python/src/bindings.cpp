// SPDX-License-Identifier: Apache-2.0
// The groomlab_core Python module: GroomLab's core (graph, strands, scalp,
// guides and hierarchy, brushes, grooms and groom files) with no Maya. The
// bindings are split by area into the bind_*.cpp files.

#include "bind_common.h"

#include "groomlab/version.h"

PYBIND11_MODULE(groomlab_core, m) {
    m.doc() =
        "GroomLab core (Maya-independent): build, edit and evaluate a groom from Python.\n\n"
        "Typical use: make a ScalpMesh, a GuideHierarchy (add_group, add_guide, generate_children, evaluate), "
        "then either a GroomSession(GroomData(graph_json, hierarchy.to_json(), scalp)) (also called Groom) kept "
        "evaluated through its setters and view(), or evaluate()d at a density and time, "
        "or a GraphDocument with ExternalInputs.set_groom(...) and an EvalContext for node-level control.";
    m.attr("__version__") = groomlab::kVersionString;
    m.attr("has_tbb") = static_cast<bool>(GROOMLAB_HAS_TBB);

    groomlab::py_bind::bindBasic(m);
    groomlab::py_bind::bindStrands(m);
    groomlab::py_bind::bindHierarchy(m);
    groomlab::py_bind::bindGraph(m);
    groomlab::py_bind::bindGroom(m);
    groomlab::py_bind::bindSession(m);
    groomlab::py_bind::bindDisplay(m);
    groomlab::py_bind::bindStroke(m);
    groomlab::py_bind::bindAlembic(m);
    groomlab::py_bind::bindReference(m);
    groomlab::py_bind::bindCardBlocks(m);
}
