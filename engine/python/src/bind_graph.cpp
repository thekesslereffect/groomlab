// SPDX-License-Identifier: Apache-2.0
// The operator graph: ports, parameters, operators and the registry,
// external inputs, the evaluation context, Graph and GraphDocument.

#include "bind_common.h"

#include <limits>
#include <sstream>

#include "groomlab/param_conditions.h"
#include "groomlab/attributes.h"
#include "groomlab/braid_tubes.h"
#include "groomlab/display.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"
#include "groomlab/graph_arrange.h"
#include "groomlab/groom.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/paths.h"
#include "groomlab/node_catalog.h"
#include "groomlab/node_library.h"
#include "groomlab/presets.h"
#include "groomlab/process_caches.h"
#include "groomlab/shortcuts.h"

namespace groomlab::py_bind {

namespace {

// EvalContext owning its path resolver (the core keeps a pointer to it).
struct PyEvalContext : EvalContext {
    PathResolver resolver;
    std::vector<std::string> searchPaths;
    py::object externalsRef;  // keeps the ExternalInputs alive

    PyEvalContext() = default;
    PyEvalContext(const PyEvalContext&) = delete;
    PyEvalContext& operator=(const PyEvalContext&) = delete;

    void setSearchPaths(std::vector<std::string> dirs) {
        searchPaths = std::move(dirs);
        if (searchPaths.empty()) {
            resolver = {};
            paths = nullptr;
        } else {
            resolver = searchPathResolver(searchPaths);
            paths = &resolver;
        }
    }
    void setExternals(py::object ext) {
        if (ext.is_none()) {
            externalsRef = py::none();
            externals = nullptr;
        } else {
            externals = &ext.cast<ExternalInputs&>();
            externalsRef = std::move(ext);
        }
    }
};

const EvalContext& contextOr(const PyEvalContext* ctx) {
    static const EvalContext defaultContext{};
    return ctx ? static_cast<const EvalContext&>(*ctx) : defaultContext;
}

// Stores a param, coerced to the kind the operator declares: ints become
// floats for float params, numbers become constant ramps for ramp params.
void setParamCoerced(Operator& op, const std::string& name, py::handle value) {
    ParamValue v;
    if (!loadParamValue(value, v)) {
        throw py::type_error("parameter '" + name + "': expected bool, int, float, str, (x, y, z), Ramp or MapRef");
    }
    for (const ParamSpec& spec : allParamSpecs(op)) {
        if (spec.name != name) continue;
        if (std::holds_alternative<double>(spec.defaultValue) && std::holds_alternative<std::int64_t>(v)) {
            v = static_cast<double>(std::get<std::int64_t>(v));
        } else if (std::holds_alternative<std::int64_t>(spec.defaultValue) && std::holds_alternative<double>(v)) {
            v = static_cast<std::int64_t>(std::get<double>(v));
        } else if (std::holds_alternative<Ramp>(spec.defaultValue)) {
            if (std::holds_alternative<double>(v)) v = Ramp::constant(static_cast<float>(std::get<double>(v)));
            else if (std::holds_alternative<std::int64_t>(v)) v = Ramp::constant(static_cast<float>(std::get<std::int64_t>(v)));
        } else if (std::holds_alternative<bool>(spec.defaultValue) && std::holds_alternative<std::int64_t>(v)) {
            v = std::get<std::int64_t>(v) != 0;
        }
        break;
    }
    op.params().set(name, std::move(v));
}

std::size_t resolveSlot(const Graph& g, NodeId dst, py::handle input) {
    if (PyLong_Check(input.ptr())) return input.cast<std::size_t>();
    const std::string name = input.cast<std::string>();
    const std::vector<PortSpec> specs = g.inputSpecs(dst);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].name == name) return i;
    }
    throw GraphError("node has no input named '" + name + "'");
}

// An output by index or name (None: the first).
std::size_t resolveOutput(const Graph& g, NodeId src, py::handle output) {
    if (output.is_none()) return 0;
    if (PyLong_Check(output.ptr())) return output.cast<std::size_t>();
    return g.outputIndex(src, output.cast<std::string>());
}

py::dict paramSpecDict(const ParamSpec& s) {
    py::dict d;
    d["name"] = s.name;
    d["kind"] = paramKind(s.defaultValue);
    d["default"] = castParamValue(s.defaultValue);
    d["description"] = s.description;
    d["unit"] = s.unit;
    if (!s.unitWhen.param.empty()) {
        py::dict when;
        when["param"] = s.unitWhen.param;
        when["in"] = s.unitWhen.in;
        d["unit_when"] = when;
    } else {
        d["unit_when"] = py::none();
    }
    d["choices"] = s.choices;
    d["animatable"] = s.animatable;
    d["group"] = s.group;
    d["advanced"] = s.advanced;
    d["world_direction"] = s.worldDirection;
    d["channel"] = std::string(channelUseName(s.channel));
    d["min"] = s.min ? py::cast(*s.min) : py::none();
    d["max"] = s.max ? py::cast(*s.max) : py::none();
    d["soft_min"] = s.softMin ? py::cast(*s.softMin) : py::none();
    d["soft_max"] = s.softMax ? py::cast(*s.softMax) : py::none();
    return d;
}

}  // namespace

namespace {

// A graph's JSON as groomlab_core.GraphPins holds it (graph_pins.h's functions take it).
struct GraphPins {
    nlohmann::json graph;
};
// And as groomlab_core.GraphPortals holds it (graph_portals.h's).
struct GraphPortals {
    nlohmann::json graph;
};

}  // namespace

void bindGraph(py::module_& m) {
    py::enum_<PortType>(m, "PortType", "What a port carries.")
        .value("Strands", PortType::Strands)
        .value("Guides", PortType::Guides)
        .value("Mesh", PortType::Mesh)
        .value("Field", PortType::Field)
        .value("Curve", PortType::Curve)
        .value("Curves", PortType::Curves)
        .value("Value", PortType::Value);
    m.def("port_types_match", &portTypesMatch, py::arg("out"), py::arg("in"), "Whether an output type can feed an input type.");
    m.def("port_type_name", &portTypeName, py::arg("type"));
    m.def("renamed_color_source", &renamedColorSource, py::arg("source"), py::arg("old"), py::arg("new"),
          "A color source naming node `old` ('<channel>@<old>') once it is renamed `new`; any other as it is.");
    m.def("braid_tube_names", &braidTubeNames, py::arg("shown"),
          "Show Tubes: the Braid nodes named in a display setting's text (DisplaySettings.braid_tubes: one a line).");
    m.def("braid_tubes_text", &braidTubesText, py::arg("names"), "braid_tube_names' reverse: the names, one a line.");
    m.def("renamed_braid_tubes", &renamedBraidTubes, py::arg("shown"), py::arg("old"), py::arg("new"),
          "Show Tubes' names once node `old` is renamed `new` (the same when it was not shown).");
    m.def("braid_section_color", &braidSectionColor, py::arg("section"),
          "A braid section's color in Show Tubes (linear RGB): Clump Colors' color of that number.");
    m.def("set_verify_incremental", &Graph::setVerifyIncremental, py::arg("on"),
          "Every incremental evaluation checked against a full one (GraphError when they differ): tests, benchmarks.");
    m.def("release_process_caches", &releaseProcessCaches,
          "Lets the process-wide caches go (distance fields, surface graphs, subdivision plans, textures, array pools): "
          "a host's scene closed. What sessions hold stays.");
    m.def("process_cache_bytes", &processCacheBytes, "What the process-wide caches hold, in bytes (an estimate).");
    py::enum_<ValueKind>(m, "ValueKind", "What a Value port holds.")
        .value("Any", ValueKind::Any)
        .value("Number", ValueKind::Number)
        .value("Integer", ValueKind::Integer)
        .value("Bool", ValueKind::Bool)
        .value("Vec3", ValueKind::Vec3)
        .value("Choice", ValueKind::Choice)
        .value("Text", ValueKind::Text);
    m.def("value_kinds_match", &valueKindsMatch, py::arg("out"), py::arg("in"),
          "Whether a Value output of one kind can feed a Value input of another.");
    m.def("value_kind_name", &valueKindName, py::arg("kind"));

    py::class_<PortSpec>(m, "PortSpec", "An input or output port: name, type, optional, and a Value port's kind.")
        .def(py::init<>())
        .def_readwrite("name", &PortSpec::name)
        .def_readwrite("type", &PortSpec::type)
        .def_readwrite("optional", &PortSpec::optional)
        .def_readwrite("value_kind", &PortSpec::valueKind)
        .def("__repr__", [](const PortSpec& p) {
            const std::string kind = p.type == PortType::Value ? std::string(" ") + valueKindName(p.valueKind) : std::string();
            return std::string("<PortSpec '") + p.name + "' " + portTypeName(p.type) + kind + (p.optional ? " optional>" : ">");
        });

    py::class_<ParamSpec>(m, "ParamSpec", "A parameter's name, default, description, unit and choices.")
        .def_readonly("name", &ParamSpec::name)
        .def_property_readonly("default", [](const ParamSpec& s) { return castParamValue(s.defaultValue); })
        .def_property_readonly("kind", [](const ParamSpec& s) { return paramKind(s.defaultValue); },
                               "bool, int, float, string, vec3, ramp or map.")
        .def_readonly("description", &ParamSpec::description)
        .def_readonly("unit", &ParamSpec::unit)
        .def_readonly("choices", &ParamSpec::choices)
        .def("to_dict", &paramSpecDict)
        .def("__repr__", [](const ParamSpec& s) { return "<ParamSpec '" + s.name + "' " + paramKind(s.defaultValue) + ">"; });

    py::class_<ParamSet>(m, "ParamSet",
                         "An operator's parameters by name. Values are bool, int, float, str, Vec3, Ramp or MapRef.")
        .def(py::init<>())
        .def("set", [](ParamSet& p, const std::string& name, const ParamValue& v) { p.set(name, v); }, py::arg("name"), py::arg("value"))
        .def("has", &ParamSet::has, py::arg("name"))
        .def("get", &ParamSet::get, py::arg("name"), "The value (GraphError when unknown).")
        .def("get_bool", &ParamSet::getBool, py::arg("name"), py::arg("fallback") = false)
        .def("get_int", &ParamSet::getInt, py::arg("name"), py::arg("fallback") = 0)
        .def("get_float", &ParamSet::getDouble, py::arg("name"), py::arg("fallback") = 0.0)
        .def("get_string", &ParamSet::getString, py::arg("name"), py::arg("fallback") = std::string{})
        .def("get_vec3", &ParamSet::getVec3, py::arg("name"), py::arg("fallback") = Vec3f{})
        .def("get_ramp", &ParamSet::getRamp, py::arg("name"), py::arg("fallback") = Ramp::constant(1.0f),
             "A ramp, or a constant ramp when the param is a number.")
        .def("values", [](const ParamSet& p) { return p.values(); }, "Every parameter as a dict.")
        .def("keys", [](const ParamSet& p) {
                std::vector<std::string> out;
                for (const auto& kv : p.values()) out.push_back(kv.first);
                return out;
            })
        .def("hash", &ParamSet::hash)
        .def("__getitem__", &ParamSet::get, py::arg("name"))
        .def("__setitem__", [](ParamSet& p, const std::string& name, const ParamValue& v) { p.set(name, v); })
        .def("__contains__", &ParamSet::has)
        .def("__len__", [](const ParamSet& p) { return p.values().size(); })
        .def("__iter__", [](const ParamSet& p) {
                std::vector<std::string> keys;
                for (const auto& kv : p.values()) keys.push_back(kv.first);
                return py::iter(py::cast(keys));
            })
        .def("__repr__", [](const ParamSet& p) {
            std::ostringstream s;
            s << "<ParamSet";
            for (const auto& kv : p.values()) s << " " << kv.first << "=" << py::str(castParamValue(kv.second)).cast<std::string>();
            s << ">";
            return s.str();
        });

    py::class_<FieldValue, std::shared_ptr<FieldValue>>(m, "FieldValue", "A scalar per strand: what a Field port carries.")
        .def("at_root", &FieldValue::atRoot, py::arg("root"), "The value at a scalp root (before strands exist).")
        .def("at", &FieldValue::at, py::arg("strands"), py::arg("strand"), "The value for strand `strand` of a set.")
        .def("maximum", &FieldValue::maximum, "The largest value the field can take.")
        .def("validate", &FieldValue::validate, py::arg("strands"), "Raises GraphError when the field cannot be read on these strands.");
    m.def("constant_field", [](float v) { return std::const_pointer_cast<FieldValue>(constantField(v)); }, py::arg("value"),
          "The same value for every strand.");
    m.def("control_input_name", &controlInputName, py::arg("name"), "The external input a Control node named `name` reads.");

    py::class_<Operator>(m, "Operator",
                         "A graph node's operator: type, ports, parameter specs and parameters. Obtained from "
                         "Graph.node(id); edit params through set_param (coerces to the declared kind) or .params.")
        .def_property_readonly("type_name", &Operator::typeName)
        .def_property_readonly("inputs", &Operator::inputs, "The operator's own input ports.")
        .def_property_readonly("output", &Operator::output, "The primary output port (the first of outputs).")
        .def_property_readonly("outputs", &Operator::outputs, "Every output port, the primary first.")
        .def_property_readonly("param_specs", &Operator::paramSpecs)
        .def_property_readonly("external_inputs", &Operator::externalInputs, "Names of external inputs it reads.")
        .def_property_readonly("category", &Operator::category)
        .def_property_readonly("params", [](Operator& op) -> ParamSet& { return op.params(); }, py::return_value_policy::reference_internal)
        .def("apply_defaults", &Operator::applyDefaults)
        .def("set_param", &setParamCoerced, py::arg("name"), py::arg("value"),
             "Sets a parameter, coerced to the kind its spec declares (int -> float, number -> constant Ramp).")
        .def("get_param", [](const Operator& op, const std::string& name) { return op.params().get(name); }, py::arg("name"))
        .def("has_param", [](const Operator& op, const std::string& name) { return op.params().has(name); }, py::arg("name"))
        .def("cache_salt", &Operator::cacheSalt)
        .def("__repr__", [](const Operator& op) { return "<Operator " + op.typeName() + ">"; });

    py::class_<OperatorRegistry, std::unique_ptr<OperatorRegistry, py::nodelete>>(m, "OperatorRegistry",
                                                                                   "Creates operators by type name.")
        .def_static("instance", &OperatorRegistry::instance, py::return_value_policy::reference)
        .def("contains", &OperatorRegistry::contains, py::arg("type_name"))
        .def("create", &OperatorRegistry::create, py::arg("type_name"),
             "A fresh operator with default params (for inspecting specs; add nodes to a graph by type name).")
        .def("type_names", &OperatorRegistry::typeNames);
    m.def("operator_type_names", []() { return OperatorRegistry::instance().typeNames(); }, "Every registered operator type.");

    py::class_<ExternalInputs>(m, "ExternalInputs",
                               "Data the host feeds the graph by name (scalp, guides, hierarchy, parts, flow, controls), "
                               "each with a content hash so only readers are invalidated when it changes.")
        .def(py::init<>())
        .def("set", &ExternalInputs::set, py::arg("name"), py::arg("value"), py::arg("hash"),
             "Registers a value (StrandSet, GuideSet, ScalpMesh, GuideHierarchy, PartLines, FlowField, FieldValue, Ramp or None) under a name.")
        .def("find",
             [](const ExternalInputs& e, const std::string& name) -> py::object {
                 const ExternalInputs::Entry* entry = e.find(name);
                 return entry ? castValue(entry->value) : py::none();
             },
             py::arg("name"), "The value registered under a name, or None.")
        .def("hash_of",
             [](const ExternalInputs& e, const std::string& name) -> py::object {
                 const ExternalInputs::Entry* entry = e.find(name);
                 if (!entry) return py::none();
                 return py::int_(entry->hash);
             },
             py::arg("name"))
        .def("set_parent", &ExternalInputs::setParent, py::arg("parent"), py::keep_alive<1, 2>(),
             "Entries not found here are looked up in `parent`.")
        .def("set_groom",
             [](ExternalInputs& e, std::shared_ptr<ScalpMesh> scalp, std::shared_ptr<GuideSet> guides,
                std::shared_ptr<GuideHierarchy> hierarchy, std::shared_ptr<PartLines> parts, std::shared_ptr<FlowField> flow) {
                 setGroomExternals(e, scalp, guides, hierarchy, parts, flow);
             },
             py::arg("scalp") = nullptr, py::arg("guides") = nullptr, py::arg("hierarchy") = nullptr, py::arg("parts") = nullptr,
             py::arg("flow") = nullptr,
             "Registers 'scalp', 'guides', 'hierarchy', 'parts' and 'flow' with their content hashes (None entries are skipped). "
             "Call again after editing any of them.")
        .def("set_controls", &setControlExternals, py::arg("controls"), "Registers Control node values by name ({name: value}).");

    py::class_<PyEvalContext>(m, "EvalContext",
                              "How a graph is evaluated: preview density, time, point budget, segments, cards, the "
                              "external inputs and where relative file paths resolve.")
        .def(py::init([](float density, double time, py::object externals, bool buildCards, double maxPoints, int segments) {
                 auto ctx = std::make_unique<PyEvalContext>();
                 ctx->previewDensity = density;
                 ctx->time = time;
                 ctx->buildCards = buildCards;
                 ctx->maxPoints = maxPoints;
                 ctx->strandSegments = segments;
                 ctx->setExternals(std::move(externals));
                 return ctx;
             }),
             py::arg("preview_density") = 1.0f, py::arg("time") = 0.0, py::arg("externals") = py::none(),
             py::arg("build_cards") = false, py::arg("max_points") = 0.0, py::arg("strand_segments") = 0)
        .def_readwrite("preview_density", &PyEvalContext::previewDensity, "1.0 for render, < 1 for interactive preview.")
        .def_readwrite("time", &PyEvalContext::time, "Seconds.")
        .def_readwrite("frames_per_second", &PyEvalContext::framesPerSecond, "The rate expressions count $frame at.")
        .def_readwrite("viewport", &PyEvalContext::viewport,
                       "An evaluation for the viewport: renderOnly nodes are bypassed (else viewportOnly ones).")
        .def_readwrite("build_cards", &PyEvalContext::buildCards, "Cards nodes build their cards.")
        .def_readwrite("max_points", &PyEvalContext::maxPoints, "Viewport point budget per Scatter (0: none).")
        .def_readwrite("strand_segments", &PyEvalContext::strandSegments, "Segments per scattered strand, for every Scatter (0: each Scatter's from the Resample nodes after it).")
        .def_property("externals", [](const PyEvalContext& c) { return c.externalsRef; }, &PyEvalContext::setExternals,
                      "The ExternalInputs the graph reads, or None.")
        .def_property("search_paths", [](const PyEvalContext& c) { return c.searchPaths; }, &PyEvalContext::setSearchPaths,
                      "Directories relative file paths (maps, caches) resolve against, in order.")
        .def("hash", &PyEvalContext::hash)
        .def("external", [](const PyEvalContext& c, const std::string& name) { return c.external(name); }, py::arg("name"));

    py::class_<Graph>(m, "Graph",
                      "A DAG of operators with pull-based, cached evaluation: only nodes whose inputs or params "
                      "changed re-run. Nodes are added by operator type name and referenced by integer ID.")
        .def(py::init<>())
        .def("add_node", [](Graph& g, const std::string& type) { return g.addNode(type); }, py::arg("type_name"),
             "Adds an operator by type name (GraphError when unknown); returns its ID.")
        .def("remove_node", &Graph::removeNode, py::arg("id"))
        .def("has_node", &Graph::hasNode, py::arg("id"))
        .def("node_ids", &Graph::nodeIds)
        .def("node", [](Graph& g, NodeId id) -> Operator& { return g.node(id); }, py::arg("id"),
             py::return_value_policy::reference_internal, "The node's operator.")
        .def("connect",
             [](Graph& g, NodeId src, NodeId dst, py::handle input, py::handle output) {
                 g.connect(src, dst, resolveSlot(g, dst, input), resolveOutput(g, src, output));
             },
             py::arg("src"), py::arg("dst"), py::arg("input"), py::arg("output") = py::none(),
             "Connects src's output (an index or name; None: the first) to dst's input (a slot index or port "
             "name). GraphError on a mismatch, an unknown port or a cycle.")
        .def("disconnect", [](Graph& g, NodeId dst, py::handle input) { g.disconnect(dst, resolveSlot(g, dst, input)); },
             py::arg("dst"), py::arg("input"))
        .def("input_source",
             [](const Graph& g, NodeId dst, py::handle input) -> py::object {
                 NodeId src = 0;
                 if (!g.inputSource(dst, resolveSlot(g, dst, input), &src)) return py::none();
                 return py::int_(src);
             },
             py::arg("dst"), py::arg("input"), "The node feeding an input, or None.")
        .def("input_output",
             [](const Graph& g, NodeId dst, py::handle input) -> py::object {
                 NodeId src = 0;
                 std::size_t output = 0;
                 if (!g.inputSource(dst, resolveSlot(g, dst, input), &src, &output)) return py::none();
                 return py::int_(output);
             },
             py::arg("dst"), py::arg("input"), "Which output of its source feeds an input (an index), or None.")
        .def("output_specs", &Graph::outputSpecs, py::arg("id"), "The node's output ports, the primary first.")
        .def("output_index", &Graph::outputIndex, py::arg("id"), py::arg("name"))
        .def("set_param_inputs", &Graph::setParamInputs, py::arg("id"), py::arg("params"),
             "Exposes params as input slots after the operator's own (Field, Curve or Value ports).")
        .def("param_inputs", &Graph::paramInputs, py::arg("id"))
        .def("param_driven", &Graph::paramDriven, py::arg("id"), py::arg("param"),
             "Whether a Value input is plugged into the param.")
        .def("input_specs", &Graph::inputSpecs, py::arg("id"), "The operator's inputs followed by one port per exposed param.")
        .def("set_bypass", &Graph::setBypass, py::arg("id"), py::arg("bypass"))
        .def("bypassed", &Graph::bypassed, py::arg("id"))
        .def("set_evaluation",
             [](Graph& g, NodeId id, const std::string& mode) { g.setEvaluationMode(id, evaluationModeFromName(mode)); },
             py::arg("id"), py::arg("mode"), "When the node runs: 'always', 'viewportOnly' or 'renderOnly'.")
        .def("evaluation", [](const Graph& g, NodeId id) { return std::string(evaluationModeName(g.evaluationMode(id))); },
             py::arg("id"))
        .def("set_label", &Graph::setLabel, py::arg("id"), py::arg("label"))
        .def("label", &Graph::label, py::arg("id"))
        .def("last_evaluation_times", &Graph::lastEvaluationTimes, "{node id: milliseconds} of the last run of each node.")
        .def("output_type", &Graph::outputType, py::arg("id"), py::arg("output") = 0)
        .def("cached_channels",
             [](const Graph& g, NodeId id) -> py::object {
                 std::vector<std::string> names;
                 if (!g.cachedChannels(id, &names)) return py::none();
                 return py::cast(names);
             },
             py::arg("id"), "Strand channel names in a node's cached result, or None without one.")
        .def("evaluate",
             [](Graph& g, NodeId id, const PyEvalContext* ctx) {
                 Value v;
                 {
                     py::gil_scoped_release release;
                     v = g.evaluate(id, contextOr(ctx));
                 }
                 return v;
             },
             py::arg("id"), py::arg("ctx") = nullptr,
             "Evaluates a node; returns its value (StrandSet, GuideSet, ScalpMesh, FieldValue, Ramp... or None).")
        .def("evaluate_output",
             [](Graph& g, NodeId id, py::handle output, const PyEvalContext* ctx) {
                 const std::size_t index = resolveOutput(g, id, output);
                 Value v;
                 {
                     py::gil_scoped_release release;
                     v = g.evaluateOutput(id, index, contextOr(ctx));
                 }
                 return v;
             },
             py::arg("id"), py::arg("output"), py::arg("ctx") = nullptr,
             "Evaluates one output of a node (an index or name); the node runs once for all its outputs.")
        .def("part_lines",
             [](Graph& g, NodeId id, const PyEvalContext* ctx) -> py::object {
                 if (g.node(id).typeName() != op::kPart) throw GraphError("part_lines: not a Part node");
                 NodeId src = 0;
                 std::size_t output = 0;
                 if (!g.inputSource(id, 0, &src, &output)) return py::none();
                 const EvalContext& c = contextOr(ctx);
                 PartsPtr lines;
                 {
                     py::gil_scoped_release release;
                     const StrandsPtr in = valueAs<StrandsPtr>(g.evaluateOutput(src, output, c));
                     const MeshPtr scalp = valueAs<MeshPtr>(c.external("scalp"));
                     if (in && scalp) lines = partLinesFor(g.node(id).params(), *in, *scalp, valueAs<PartsPtr>(c.external("parts")));
                 }
                 if (!lines) return py::none();
                 return py::cast(std::const_pointer_cast<PartLines>(lines));
             },
             py::arg("id"), py::arg("ctx") = nullptr,
             "The parts a Part node opens (PartLines, None without any): with partsFrom auto the ones found in "
             "the strands it takes, else the groom's painted ones. For drawing a selected Part's parts.")
        .def("braid_tubes",
             [](Graph& g, NodeId id, const PyEvalContext* ctx, int around) {
                 BraidTubes tubes;
                 {
                     py::gil_scoped_release release;
                     tubes = braidTubes(g, id, contextOr(ctx), around);
                 }
                 auto points = [](const std::vector<Vec3f>& from) {
                     py::list out;
                     for (const Vec3f& p : from) out.append(toTuple(p));
                     return out;
                 };
                 py::list list;
                 for (const BraidTubes::Tube& t : tubes.tubes) {
                     py::dict tube;
                     tube["path"] = t.path;
                     tube["braidId"] = t.braidId;
                     tube["section"] = t.section;
                     tube["at"] = t.at;
                     tube["centers"] = points(t.centers);
                     tube["across"] = points(t.across);
                     tube["thick"] = points(t.thick);
                     tube["firstPoint"] = t.firstPoint;
                     tube["firstQuad"] = t.firstQuad;
                     list.append(tube);
                 }
                 py::list quads;
                 for (const auto& q : tubes.quads) quads.append(py::make_tuple(q[0], q[1], q[2], q[3]));
                 py::dict out;
                 out["around"] = tubes.around;
                 out["pressed"] = tubes.pressed;
                 out["tubes"] = list;
                 out["points"] = points(tubes.points);
                 out["quads"] = quads;
                 out["quadTube"] = tubes.quadTube;
                 return out;
             },
             py::arg("id"), py::arg("ctx") = nullptr, py::arg("around") = 12,
             "A Braid node's sections as tubes along its paths (braid_tubes.h): {around, pressed, tubes: [{path, braidId, "
             "section, at, centers, across, thick, firstPoint, firstQuad}], points, quads, quadTube}: per path and section "
             "a center line with its ellipse's half axes at each ring, and all of them as one quad mesh (around points a "
             "ring, open ends; pressed, a version 2 node's: each ring on its section's pressed outline). Its inputs are "
             "evaluated through the graph; the Braid itself is not. GraphError for a node that is not a Braid.")
        .def("evaluate_strands",
             [](Graph& g, NodeId id, const PyEvalContext* ctx) -> py::object {
                 StrandsPtr s;
                 {
                     py::gil_scoped_release release;
                     s = g.evaluateStrands(id, contextOr(ctx));
                 }
                 if (!s) return py::none();
                 return py::cast(std::make_shared<StrandSet>(*s));
             },
             py::arg("id"), py::arg("ctx") = nullptr, "Evaluates a node as strands (a copy-on-write copy), or None.")
        .def("clear_cache", &Graph::clearCache)
        .def("adopt_cache", &Graph::adoptCache, py::arg("previous"), "Takes another graph's cached results for the next evaluation.")
        .def("set_cache_intermediates", &Graph::setCacheIntermediates, py::arg("enabled"))
        .def_property("cache_limit", &Graph::cacheLimit, &Graph::setCacheLimit,
                      "Bytes the cached results may hold (0: no limit); past it results go after each evaluation.")
        .def_property_readonly("cached_bytes", &Graph::cachedBytes, "Bytes the cached strands hold (each array once).")
        .def_property_readonly("cache_evictions", &Graph::cacheEvictions, "Results the cache limit let go so far.")
        .def_property_readonly("evaluation_count", &Graph::evaluationCount, "Operator evaluations so far (cache effectiveness).")
        .def("__len__", [](const Graph& g) { return g.nodeIds().size(); })
        .def("__repr__", [](const Graph& g) { return "<Graph nodes=" + std::to_string(g.nodeIds().size()) + ">"; });

    py::class_<GraphDocument>(m, "GraphDocument",
                              "A graph plus the node names of its JSON form and its output node. Load with "
                              "load_graph_json; add and connect nodes by name; evaluate() runs the output node.")
        .def(py::init([]() { return std::make_unique<GraphDocument>(loadGraphJson({})); }), "The default graph.")
        .def_property_readonly("graph", [](GraphDocument& d) -> Graph& { return d.graph; }, py::return_value_policy::reference_internal)
        .def_property_readonly("nodes_by_name", [](const GraphDocument& d) { return d.nodesByName; }, "{name: node id}.")
        .def_property_readonly("names", [](const GraphDocument& d) {
                std::vector<std::string> out;
                for (const auto& kv : d.nodesByName) out.push_back(kv.first);
                return out;
            })
        .def_property("output", [](const GraphDocument& d) { return d.output; }, [](GraphDocument& d, NodeId id) { d.output = id; },
                      "The node whose result is the groom (ID).")
        .def("set_output", [](GraphDocument& d, const std::string& name) { d.output = d.node(name); }, py::arg("name"))
        .def("node", &GraphDocument::node, py::arg("name"), "Node ID by name (GraphError if missing).")
        .def("name_of", &GraphDocument::nameOf, py::arg("id"))
        .def("has", [](const GraphDocument& d, const std::string& name) { return d.nodesByName.count(name) != 0; }, py::arg("name"))
        .def("op", [](GraphDocument& d, const std::string& name) -> Operator& { return d.graph.node(d.node(name)); }, py::arg("name"),
             py::return_value_policy::reference_internal, "The operator of the node named `name`.")
        .def("add_node",
             [](GraphDocument& d, const std::string& name, const std::string& type) {
                 if (d.nodesByName.count(name)) throw GraphError("a node named '" + name + "' already exists");
                 const NodeId id = d.graph.addNode(type);
                 d.nodesByName[name] = id;
                 d.graph.setLabel(id, name);
                 return id;
             },
             py::arg("name"), py::arg("type_name"), "Adds a named node; returns its ID.")
        .def("remove_node",
             [](GraphDocument& d, const std::string& name) {
                 const NodeId id = d.node(name);
                 d.graph.removeNode(id);
                 d.nodesByName.erase(name);
                 if (d.output == id) d.output = 0;
             },
             py::arg("name"))
        .def("rename_node",
             [](GraphDocument& d, const std::string& from, const std::string& to) {
                 if (d.nodesByName.count(to)) throw GraphError("a node named '" + to + "' already exists");
                 const NodeId id = d.node(from);
                 d.nodesByName.erase(from);
                 d.nodesByName[to] = id;
                 d.graph.setLabel(id, to);
             },
             py::arg("old"), py::arg("new"))
        .def("connect",
             [](GraphDocument& d, const std::string& src, const std::string& dst, py::handle input, py::handle output) {
                 const NodeId from = d.node(src), to = d.node(dst);
                 d.graph.connect(from, to, resolveSlot(d.graph, to, input), resolveOutput(d.graph, from, output));
             },
             py::arg("src"), py::arg("dst"), py::arg("input"), py::arg("output") = py::none(),
             "Connects two named nodes; `input` is a port name or slot index, `output` src's output (name or "
             "index; None: the first).")
        .def("disconnect",
             [](GraphDocument& d, const std::string& dst, py::handle input) {
                 const NodeId to = d.node(dst);
                 d.graph.disconnect(to, resolveSlot(d.graph, to, input));
             },
             py::arg("dst"), py::arg("input"))
        .def("evaluate",
             [](GraphDocument& d, const PyEvalContext* ctx) -> py::object {
                 if (d.output == 0) throw GraphError("the document has no output node");
                 StrandsPtr s;
                 {
                     py::gil_scoped_release release;
                     s = d.graph.evaluateStrands(d.output, contextOr(ctx));
                 }
                 if (!s) return py::none();
                 return py::cast(std::make_shared<StrandSet>(*s));
             },
             py::arg("ctx") = nullptr, "Evaluates the output node as strands.")
        .def(
            "strand_resolution",
            [](const GraphDocument& d, const std::string& name, bool viewport) {
                const Graph::Resolution r = d.graph.strandResolution(d.node(name), viewport);
                return py::make_tuple(r.segments, r.from ? py::cast(d.nameOf(r.from)) : py::none());
            },
            py::arg("name"), py::arg("viewport") = true,
            "(segments, resample name or None): the most segments any Resample downstream of the node asks for (0 and "
            "None: none; a Scatter there grows 8).")
        .def(
            "strand_resolution_note",
            [](const GraphDocument& d, const std::string& name, bool viewport) { return d.graph.strandResolutionNote(d.node(name), viewport); },
            py::arg("name"), py::arg("viewport") = true,
            "A Scatter's or Resample's strand resolution in plain words (which Resample sets it); '' for other nodes.")
        .def("to_json", [](const GraphDocument& d) { return saveGraphJson(d); })
        .def("node_times",
             [](const GraphDocument& d) {
                 std::vector<std::pair<std::string, double>> out;
                 for (const auto& kv : d.graph.lastEvaluationTimes()) out.emplace_back(d.nameOf(kv.first), kv.second);
                 return out;
             },
             "[(node name, milliseconds)] of the last run of each node.")
        .def("__repr__", [](const GraphDocument& d) { return "<GraphDocument nodes=" + std::to_string(d.nodesByName.size()) + ">"; });

    m.def("load_graph_json", [](py::handle text) { return loadGraphJson(jsonText(text)); }, py::arg("text"),
          "Parses a graph from JSON text (or a dict). Empty text or '{}' gives the default graph. Raises GraphError.");
    m.def("save_graph_json", &saveGraphJson, py::arg("document"));
    m.def("default_graph_json", []() { return defaultGraphJson(); }, "Scalp -> Scatter -> Interpolate (with Guides) -> Length -> Width -> Resample -> Output, as JSON.");
    m.def("default_graph", []() { return jsonLoads(defaultGraphJson()); }, "The default graph parsed into Python objects.");
    m.def("operator_specs_json", &operatorSpecsJson, "Every registered operator (ports, params) as JSON text.");
    m.def("operator_specs", []() { return jsonLoads(operatorSpecsJson()); }, "Every registered operator (ports, params) as a list of dicts.");
    m.def(
        "node_categories",
        []() {
            py::list out;
            for (const NodeCategory& c : nodeCategories()) {
                py::dict d;
                d["id"] = c.id;
                d["title"] = c.title;
                out.append(d);
            }
            return out;
        },
        "The add-node menu's categories in order: [{id, title}] (node_catalog.h; each spec's category is an id).");
    m.def(
        "shortcuts",
        [](const std::string& host) {
            const unsigned mask = host == "maya" ? kShortcutMaya : host == "app" ? kShortcutApp : host == "blender" ? kShortcutBlender : kShortcutAll;
            py::list out;
            for (const Shortcut& s : shortcutsFor(mask)) {
                py::dict d;
                d["id"] = s.id;
                d["context"] = s.context;
                d["keys"] = s.keys;
                d["action"] = s.action;
                d["hotkey_set"] = s.mayaHotkeySet;
                out.append(d);
            }
            return out;
        },
        py::arg("host") = std::string("maya"),
        "Every host's shortcuts in one table (shortcuts.h): [{id, context, keys, action, hotkey_set}] for host "
        "'maya', 'app' or 'blender' ('' : all). hotkey_set: Maya binds it with the GroomLab hotkey set.");

    // Controls every host makes and reads (graph_controls.h): Control nodes' sliders, Expression
    // variables with a control, keyed params.
    m.def("control_name", &controlName, py::arg("wanted"), "A control's name from any text (letters, digits, _).");
    m.def(
        "param_sections", [](const std::string& paramsJson) { return paramSections(nlohmann::json::parse(paramsJson)); },
        py::arg("params_json"),
        "The Node panel's section of each param of a spec's params (param_conditions.h, every host's): "
        "'Advanced', its group when there are two groups or more, else ''.");
    m.def(
        "param_shown",
        [](const std::string& paramsJson, const std::string& name, const std::string& valuesJson) {
            const nlohmann::json values = nlohmann::json::parse(valuesJson);
            return paramShown(nlohmann::json::parse(paramsJson), name, [&](const std::string& param) {
                const auto it = values.find(param);
                return it == values.end() ? nlohmann::json() : *it;
            });
        },
        py::arg("params_json"), py::arg("name"), py::arg("values_json"),
        "Whether a param shows: its showWhen holds for the node's values ({param: value}), and up the chain.");
    m.def(
        "node_search_rank",
        [](const std::string& text, const py::dict& words) {
            NodeSearchWords w;
            auto get = [&](const char* key) { return words.contains(key) ? py::cast<std::string>(words[key]) : std::string(); };
            w.name = get("name");
            w.label = get("label");
            w.keywords = get("keywords");
            w.params = get("params");
            w.about = get("about");
            w.details = get("details");
            const int rank = nodeSearchRank(text, w);
            return rank < 0 ? py::object(py::none()) : py::object(py::int_(rank));
        },
        py::arg("text"), py::arg("words"),
        "The Add Node search's rank of an entry ({name, label, keywords, params, about, details}, lower case), "
        "None for no match (node_catalog.h nodeSearchRank, every host's).");
    m.def(
        "node_search_words",
        [](const std::string& specJson) {
            const NodeSearchWords w = nodeSearchWords(specJson);
            py::dict out;
            out["name"] = w.name;
            out["label"] = w.label;
            out["keywords"] = w.keywords;
            out["params"] = w.params;
            out["about"] = w.about;
            out["details"] = w.details;
            return out;
        },
        py::arg("spec_json"), "An operator spec's search words (node_catalog.h nodeSearchWords).");
    m.def("condition_text", [](const std::string& valueJson) { return conditionText(nlohmann::json::parse(valueJson)); },
          py::arg("value_json"),
          "A param's value as a showWhen/unitWhen condition reads it (param_conditions.h, every host's): "
          "true/false, a string as it is, a whole number without its .0.");
    m.def("controls_json", &controlsJson, py::arg("graph_json"),
          "Every control the graph reads: {name: {kind, node, default, ...}} as JSON (graph_controls.h).");

    // A graph's pins, the reroute points its links are drawn through (graph_pins.h): every host's rules
    // for which survive a graph edit, their order and joining. Maya's GraphModel keeps its pins through this.
    py::class_<GraphPins>(m, "GraphPins",
                          "The pins of a graph's JSON (its \"connections\" and \"pins\": {points, routes}), read and edited as "
                          "every host does (graph_pins.h).")
        .def(py::init([](const std::string& graphJson) {
                 GraphPins p{nlohmann::json::parse(graphJson, nullptr, false)};
                 if (!p.graph.is_object()) throw GraphError("the graph is not a JSON object");
                 return p;
             }),
             py::arg("graph_json"))
        .def("pins_json", [](const GraphPins& p) { return p.graph.contains("pins") ? p.graph["pins"].dump() : std::string(); },
             "The graph's \"pins\" as JSON, in order (\"\" when it has none).")
        .def("prune", [](GraphPins& p, bool oneFeed) { prunePins(p.graph, oneFeed); }, py::arg("one_feed") = false,
             "Drops the routes whose link is gone or comes from elsewhere now and the pins no route passes. one_feed: only "
             "the routes a pin with one input (Blender's reroute) can draw.")
        .def("ids", [](const GraphPins& p) { return pinIds(p.graph); }, "The pin ids, by number.")
        .def("position",
             [](const GraphPins& p, const std::string& pin) -> py::object {
                 double x = 0.0, y = 0.0;
                 if (!pinPosition(p.graph, pin, &x, &y)) return py::none();
                 return py::make_tuple(x, y);
             },
             py::arg("pin"), "(x, y), or None for no such pin.")
        .def("route", [](const GraphPins& p, const std::string& to, const std::string& input) { return pinRoute(p.graph, to, input); },
             py::arg("to"), py::arg("input"), "The pins the link into to.input runs through, source first.")
        .def("source",
             [](const GraphPins& p, const std::string& pin) -> py::object {
                 const std::optional<PinSource> source = pinSource(p.graph, pin);
                 if (!source) return py::none();
                 return py::make_tuple(source->from, source->output);
             },
             py::arg("pin"), "(from, output) of the signal a pin carries (output \"\": the source's first), or None.")
        .def("links",
             [](const GraphPins& p, const std::string& pin) {
                 py::list out;
                 for (const PinLink& l : pinLinks(p.graph, pin)) out.append(py::make_tuple(py::make_tuple(l.to, l.input), l.index));
                 return out;
             },
             py::arg("pin"), "[((to, input), index)]: the links through a pin and its place in each one's route.")
        .def("route_to", [](const GraphPins& p, const std::string& pin) { return pinRouteTo(p.graph, pin); }, py::arg("pin"),
             "The pins from the source up to and including `pin`: what a link drawn out of it runs through.")
        .def("add",
             [](GraphPins& p, const std::string& to, const std::string& input, double x, double y, int index) {
                 return addPin(p.graph, to, input, x, y, index);
             },
             py::arg("to"), py::arg("input"), py::arg("x"), py::arg("y"), py::arg("index") = -1,
             "A new pin on the link into to.input, at `index` in its route (-1: last); its id, \"\" when nothing feeds to.input.")
        .def("move", [](GraphPins& p, const std::string& pin, double x, double y) { return movePin(p.graph, pin, x, y); }, py::arg("pin"),
             py::arg("x"), py::arg("y"))
        .def("remove", [](GraphPins& p, const std::string& pin) { return removePin(p.graph, pin); }, py::arg("pin"),
             "The pin out of every route through it.")
        .def("merge", [](GraphPins& p, const std::string& keep, const std::string& drop) { return mergePins(p.graph, keep, drop); },
             py::arg("keep"), py::arg("drop"), "Routes through `drop` run through `keep` instead: only pins of one signal join.")
        .def("set_route",
             [](GraphPins& p, const std::string& to, const std::string& input, const std::vector<std::string>& via) {
                 return setPinRoute(p.graph, to, input, via);
             },
             py::arg("to"), py::arg("input"), py::arg("via"), "The link into to.input runs through `via`; False when nothing feeds it.")
        .def("rename_node", [](GraphPins& p, const std::string& from, const std::string& to) { renamePinNode(p.graph, from, to); },
             py::arg("old"), py::arg("new"), "A node renamed: the routes from and to it follow (the connections renamed first).");

    // A graph's portals, teleporting links (graph_portals.h): every host's rules. Maya's GraphModel
    // keeps its portals through this.
    py::class_<GraphPortals>(m, "GraphPortals",
                             "The portals of a graph's JSON (its \"connections\" and \"portals\"), read and edited as every host "
                             "does (graph_portals.h).")
        .def(py::init([](const std::string& graphJson) {
                 GraphPortals p{nlohmann::json::parse(graphJson, nullptr, false)};
                 if (!p.graph.is_object()) throw GraphError("the graph is not a JSON object");
                 return p;
             }),
             py::arg("graph_json"))
        .def("portals_json",
             [](const GraphPortals& p) { return p.graph.contains("portals") ? p.graph["portals"].dump() : std::string(); },
             "The graph's \"portals\" as JSON (\"\" when it has none).")
        .def("prune", [](GraphPortals& p) { prunePortals(p.graph); })
        .def("names", [](const GraphPortals& p) { return portalNames(p.graph); })
        .def("portal_of", [](const GraphPortals& p, const std::string& from, const std::string& output) { return portalOf(p.graph, from, output); },
             py::arg("src"), py::arg("output") = std::string(), "The portal of a source output, \"\" none.")
        .def("portal_of_link",
             [](const GraphPortals& p, const std::string& to, const std::string& input) -> py::object {
                 const auto at = portalOfLink(p.graph, to, input);
                 if (!at) return py::none();
                 return py::make_tuple(at->portal, at->exit);
             },
             py::arg("to"), py::arg("input"), "(portal, exit) the link into to.input comes out of, or None.")
        .def("add",
             [](GraphPortals& p, const std::string& from, const std::string& output, double x, double y, const std::string& name) {
                 return addPortal(p.graph, from, output, x, y, name);
             },
             py::arg("src"), py::arg("output"), py::arg("x"), py::arg("y"), py::arg("name") = std::string())
        .def("add_exit", [](GraphPortals& p, const std::string& portal, double x, double y) { return addPortalExit(p.graph, portal, x, y); },
             py::arg("portal"), py::arg("x"), py::arg("y"))
        .def("teleport",
             [](GraphPortals& p, const std::string& to, const std::string& input, double exitX, double exitY, double entryX,
                double entryY) -> py::object {
                 const auto at = teleportLink(p.graph, to, input, exitX, exitY, entryX, entryY);
                 if (!at) return py::none();
                 return py::make_tuple(at->portal, at->exit);
             },
             py::arg("to"), py::arg("input"), py::arg("exit_x"), py::arg("exit_y"), py::arg("entry_x"), py::arg("entry_y"),
             "The link into to.input drawn through its source's portal, out of a new exit: (portal, exit), or None.")
        .def("add_link",
             [](GraphPortals& p, const std::string& portal, int exit, const std::string& to, const std::string& input) {
                 return addPortalLink(p.graph, portal, exit, to, input);
             },
             py::arg("portal"), py::arg("exit"), py::arg("to"), py::arg("input"))
        .def("remove_link", [](GraphPortals& p, const std::string& to, const std::string& input) { return removePortalLink(p.graph, to, input); },
             py::arg("to"), py::arg("input"))
        .def("move", [](GraphPortals& p, const std::string& portal, double x, double y) { return movePortal(p.graph, portal, x, y); },
             py::arg("portal"), py::arg("x"), py::arg("y"))
        .def("move_exit",
             [](GraphPortals& p, const std::string& portal, int exit, double x, double y) { return movePortalExit(p.graph, portal, exit, x, y); },
             py::arg("portal"), py::arg("exit"), py::arg("x"), py::arg("y"))
        .def("remove", [](GraphPortals& p, const std::string& portal) { return removePortal(p.graph, portal); }, py::arg("portal"))
        .def("remove_exit", [](GraphPortals& p, const std::string& portal, int exit) { return removePortalExit(p.graph, portal, exit); },
             py::arg("portal"), py::arg("exit"))
        .def("rename", [](GraphPortals& p, const std::string& portal, const std::string& name) { return renamePortal(p.graph, portal, name); },
             py::arg("portal"), py::arg("name"))
        .def("rename_node", [](GraphPortals& p, const std::string& from, const std::string& to) { renamePortalNode(p.graph, from, to); },
             py::arg("old"), py::arg("new"));

    // Arranging a graph by hand (graph_arrange.h): every host's Align, Tidy Selection, backdrops,
    // Shift+drag, shake and wire styles. Boxes are (name, x, y, w, h) tuples.
    auto boxesOf = [](const std::vector<std::tuple<std::string, double, double, double, double>>& boxes) {
        std::vector<LayoutBox> out;
        for (const auto& [name, x, y, w, h] : boxes) out.push_back({name, x, y, w, h});
        return out;
    };
    m.def("align_modes", &alignModes, "The Align modes, in the menus' order.");
    m.def("align_boxes",
          [boxesOf](const std::vector<std::tuple<std::string, double, double, double, double>>& boxes, const std::string& mode, double gap) {
              return alignBoxes(boxesOf(boxes), mode, gap);
          },
          py::arg("boxes"), py::arg("mode"), py::arg("gap") = 40.0, "{name: (x, y)}: each box's top-left aligned (graph_arrange.h).");
    m.def("arrange_selection",
          [boxesOf](const GraphEdit& edit, const std::vector<std::tuple<std::string, double, double, double, double>>& boxes, double column,
                    double row) { return arrangeSelection(edit, boxesOf(boxes), column, row); },
          py::arg("edit"), py::arg("boxes"), py::arg("column"), py::arg("row"),
          "Tidy Selection: {name: (x, y)} for the selected nodes laid out where they were.");
    m.def("grown_backdrop",
          [boxesOf](const std::tuple<double, double, double, double>& backdrop,
                    const std::vector<std::tuple<std::string, double, double, double, double>>& nodes, double margin, double header) {
              const auto [x, y, w, h] = backdrop;
              const LayoutBox out = grownBackdrop({"", x, y, w, h}, boxesOf(nodes), margin, header);
              return std::make_tuple(out.x, out.y, out.w, out.h);
          },
          py::arg("backdrop"), py::arg("nodes"), py::arg("margin"), py::arg("header"),
          "(x, y, w, h): a backdrop grown to hold the nodes (never shrunk).");
    m.def("folded_backdrop",
          [](const std::tuple<double, double, double, double>& backdrop, double header) {
              const auto [x, y, w, h] = backdrop;
              const LayoutBox out = foldedBackdrop({"", x, y, w, h}, header);
              return std::make_tuple(out.x, out.y, out.w, out.h);
          },
          py::arg("backdrop"), py::arg("header"), "(x, y, w, h): a folded backdrop's box.");
    m.def("nodes_with_upstream", &nodesWithUpstream, py::arg("edit"), py::arg("names"),
          "The nodes a Shift+drag carries: those and everything feeding them, in the graph's order.");
    py::class_<ShakeDetector>(m, "ShakeDetector", "A node shaken while dragged (screen pixels, seconds): graph_arrange.h.")
        .def(py::init<int, double, double>(), py::arg("turns") = 4, py::arg("min_travel") = 18.0, py::arg("window") = 0.7)
        .def("reset", &ShakeDetector::reset)
        .def("add", &ShakeDetector::add, py::arg("seconds"), py::arg("x"), py::arg("y"));
    m.def("wire_styles", &wireStyles, "The wire styles: smooth, square, rounded.");
    m.def("wire_route",
          [](const std::string& style, double ax, double ay, double bx, double by, std::optional<double> below, bool through) {
              py::list out;
              for (const WireStep& s : wireRoute(style, ax, ay, bx, by, below ? *below : std::nan(""), through)) {
                  if (s.cubic) out.append(py::make_tuple(s.c1x, s.c1y, s.c2x, s.c2y, s.x, s.y));
                  else out.append(py::make_tuple(s.x, s.y));
              }
              return out;
          },
          py::arg("style"), py::arg("ax"), py::arg("ay"), py::arg("bx"), py::arg("by"), py::arg("below") = py::none(),
          py::arg("through") = false,
          "A wire's steps from (ax, ay): (x, y) a line, (c1x, c1y, c2x, c2y, x, y) a cubic.");
    m.def("compound_graph_json", &compoundGraphJson, py::arg("graph_json"), py::arg("path"),
          "The graph a Compound holds (at its path, \"outer/inner\"), as a graph of its own.");
    m.def("with_compound_graph", &withCompoundGraph, py::arg("graph_json"), py::arg("path"), py::arg("inner_json"),
          "The whole graph with the compound at `path` holding `inner_json` instead (its inputs following its Input nodes).");

    // The editors' edits of a graph's JSON every host makes (graph_edit.h): Maya's GraphModel
    // and the standalone app's node graph.
    py::class_<GraphEdit>(m, "GraphEdit",
                          "A graph's JSON edited as every host's node editor edits it: nodes added with their names, "
                          "seeds and positions, removed, connected with the type and cycle checks.")
        .def(py::init<const std::string&>(), py::arg("graph_json"))
        .def("to_json", &GraphEdit::toJson)
        .def("has_node", &GraphEdit::hasNode, py::arg("name"))
        .def("node_type", &GraphEdit::nodeType, py::arg("name"))
        .def("node_names", &GraphEdit::nodeNames)
        .def("insert_simulate",
             [](GraphEdit& e, const std::string& paramsJson, std::optional<double> groomSize) { return insertSimulate(e, paramsJson, groomSize); },
             py::arg("params_json") = std::string(), py::arg("groom_size") = py::none(),
             "Add > Simulate (graph_inserts.h): a Simulate node between the guides and the Interpolates they feed.")
        .def("insert_motion",
             [](GraphEdit& e, const std::string& type, const std::string& paramsJson, std::optional<double> groomSize) {
                 return insertMotion(e, type, paramsJson, groomSize);
             },
             py::arg("type"), py::arg("params_json") = std::string(), py::arg("groom_size") = py::none(),
             "Add > Wind / Jiggle (graph_inserts.h): the node before the output, before a Collide or Cards ending the chain.")
        .def("motion_anchor", &motionAnchor, "The node a motion node goes before (graph_inserts.h).")
        .def("insert_part",
             [](GraphEdit& e, const std::string& paramsJson, const std::string& after, std::optional<double> groomSize) {
                 bool made = false;
                 std::string name = insertPart(e, paramsJson, after, groomSize, &made);
                 return py::make_tuple(name, made);
             },
             py::arg("params_json") = std::string(), py::arg("after") = std::string(), py::arg("groom_size") = py::none(),
             "(name, made): the graph's Part node, or one made after `after` (graph_inserts.h: every host's part tools).")
        .def("remove_scalp_nodes", [](GraphEdit& e, const std::string& mesh) { return removeScalpNodes(e, mesh); }, py::arg("mesh_name"),
             "Remove > Scalp's graph (graph_inserts.h): its Scalp nodes and what they alone feed; a Merge left alone passes through.")
        .def("remove_collider_nodes", [](GraphEdit& e, const std::string& mesh) { return removeColliderNodes(e, mesh); },
             py::arg("mesh_name"), "Remove > Collider's graph (graph_inserts.h): its ColliderMesh nodes, their Collide nodes passing through.")
        .def("insert_cards",
             [](GraphEdit& e, const std::string& paramsJson, std::optional<double> groomSize) {
                 bool made = false;
                 std::string name = insertCards(e, paramsJson, groomSize, &made);
                 return py::make_tuple(name, made);
             },
             py::arg("params_json") = std::string(), py::arg("groom_size") = py::none(),
             "(name, made): Add > Cards (graph_inserts.h): the first Cards node, or one made before the output when there is none.")
        .def("insert_guide_cache",
             [](GraphEdit& e, const std::string& path, const std::string& object, std::optional<double> groomSize) {
                 return insertGuideCache(e, path, object, groomSize);
             },
             py::arg("path"), py::arg("object") = std::string(), py::arg("groom_size") = py::none(),
             "Load Guide Cache (graph_inserts.h): a GuideCache node right after the Guides node, or the one there reading the file.")
        .def("insert_drive_guides",
             [](GraphEdit& e, std::optional<double> groomSize) { return insertDriveGuides(e, groomSize); },
             py::arg("groom_size") = py::none(),
             "Add Drivers (graph_inserts.h): a DriveGuides node right after the Guides node, or the one there.")
        .def("insert_collider",
             [](GraphEdit& e, const std::string& mesh, bool wire, std::optional<double> offset, std::optional<double> friction,
                std::optional<double> groomSize) { return insertCollider(e, mesh, wire, offset, friction, groomSize); },
             py::arg("mesh_name"), py::arg("wire") = true, py::arg("offset") = py::none(), py::arg("friction") = py::none(),
             py::arg("groom_size") = py::none(),
             "Add > Collider's graph (graph_inserts.h): a ColliderMesh node, and with `wire` a Collide before the output.")
        .def("insert_braid",
             [](GraphEdit& e, const std::string& group, const std::string& paramsJson, const std::string& after,
                std::optional<double> groomSize) { return insertBraid(e, group, paramsJson, after, groomSize); },
             py::arg("group") = std::string("braids"), py::arg("params_json") = std::string(), py::arg("after") = std::string(),
             py::arg("groom_size") = py::none(),
             "Add > Braid (graph_inserts.h): a Braid node after the Interpolate, its paths the group's Guides node.")
        .def("insert_feathers",
             [](GraphEdit& e, const std::vector<std::pair<std::string, std::string>>& designs, const std::vector<double>& weights,
                const std::string& source, const std::string& group, bool keepHair, std::optional<double> density,
                std::optional<double> groomSize) {
                 std::vector<FeatherDesign> list;
                 for (const auto& [name, preset] : designs) list.push_back({name, preset});
                 const FeatherNodes made = insertFeathers(e, list, weights, source, group, keepHair, density, groomSize);
                 py::dict out;
                 auto orNone = [](const std::string& s) { return s.empty() ? py::object(py::none()) : py::object(py::str(s)); };
                 out["shafts"] = made.shafts;
                 out["split"] = orNone(made.split);
                 out["designs"] = made.designs;
                 out["merges"] = made.merges;
                 out["merge"] = orNone(made.merge);
                 return out;
             },
             py::arg("designs"), py::arg("weights") = std::vector<double>(), py::arg("source") = std::string("scatter"),
             py::arg("group") = std::string("feathers"), py::arg("keep_hair") = true, py::arg("density") = py::none(),
             py::arg("groom_size") = py::none(),
             "Add > Feathers (graph_inserts.h): designs [(name, fitted preset JSON)] on shafts; returns {shafts, split, designs, "
             "merges, merge}.")
        .def("exclude_from_hair_guides", [](GraphEdit& e, const std::string& group) { return excludeFromHairGuides(e, group); },
             py::arg("group"), "The hair's Guides nodes leave guide group `group` out (graph_inserts.h); True when that changed.")
        .def("feather_groups", [](const GraphEdit& e) { return featherGroups(e); },
             "{groupName: groupId} of the feather designs in the graph (graph_inserts.h).")
        .def("feather_guide_groups", [](const GraphEdit& e) { return featherGuideGroups(e); },
             "The guide groups whose Guides nodes feed a feather design, in the graph's order (graph_inserts.h).")
        .def("insert_before",
             [](GraphEdit& e, const std::string& anchor, const std::string& node, const std::string& input,
                const std::string& nodeInput) { insertBefore(e, anchor, node, input, nodeInput); },
             py::arg("anchor"), py::arg("node"), py::arg("input") = std::string("strands"), py::arg("node_input") = std::string("strands"),
             "`node` between what feeds anchor's input and anchor, a row below it (graph_inserts.h).")
        .def("insert_between",
             [](GraphEdit& e, const std::string& src, const std::string& dst, const std::string& node, const std::string& nodeInput) {
                 insertBetween(e, src, dst, node, nodeInput);
             },
             py::arg("src"), py::arg("dst"), py::arg("node"), py::arg("node_input") = std::string("strands"),
             "Every link from src into dst through `node`, on its node_input (\"\": its first input) (graph_inserts.h).")
        .def("insert_after",
             [](GraphEdit& e, const std::string& anchor, const std::string& node, const std::string& nodeInput) {
                 insertAfter(e, anchor, node, nodeInput);
             },
             py::arg("anchor"), py::arg("node"), py::arg("node_input") = std::string(),
             "`node` right after anchor in its chain (before the first node it fed), half a column right of and a row below it "
             "(graph_inserts.h).")
        .def("insert_on_link",
             [](GraphEdit& e, const std::string& src, const std::string& dst, const std::string& input, const std::string& node,
                const std::string& output) { insertOnLink(e, src, dst, input, node, output); },
             py::arg("src"), py::arg("dst"), py::arg("input"), py::arg("node"), py::arg("output") = std::string(),
             "The one link src -> dst.input through `node`, on its first input that takes it: a node dropped on a wire "
             "(graph_inserts.h).")
        .def("insert_scalp",
             [](GraphEdit& e, const std::string& mesh, std::optional<double> groomSize) {
                 const ScalpNodes made = insertScalp(e, mesh, groomSize);
                 py::dict out;
                 auto orNone = [](const std::string& s) { return s.empty() ? py::object(py::none()) : py::object(py::str(s)); };
                 out["scalp"] = made.scalp;
                 out["scatter"] = orNone(made.scatter);
                 out["interpolate"] = orNone(made.interpolate);
                 out["merge"] = orNone(made.merge);
                 return out;
             },
             py::arg("mesh_name"), py::arg("groom_size") = py::none(),
             "Add Scalp's graph (graph_inserts.h): a Scalp node reading the mesh, a Scatter and an Interpolate as the groom's own, "
             "merged with the hair; returns {scalp, scatter, interpolate, merge} (the Scalp node alone when one reads it already).")
        .def("not_keyable", [](const GraphEdit& e, const std::string& node, const std::string& param) { return notKeyable(e, node, param); },
             py::arg("node"), py::arg("param"), "Why node.param cannot be keyed in place (\"\" when it can).")
        .def("expose_param", [](GraphEdit& e, const std::string& node, const std::string& param) { exposeParam(e, node, param); },
             py::arg("node"), py::arg("param"), "node.param as an input port (graph_controls.h).")
        .def("unexpose_param", [](GraphEdit& e, const std::string& node, const std::string& param) { return unexposeParam(e, node, param); },
             py::arg("node"), py::arg("param"),
             "node.param's input port gone with what fed it, the param keeping its own value; False when it was no port.")
        .def("compound_expose",
             [](GraphEdit& e, const std::string& compound, const std::string& name, const std::string& target) {
                 return compoundExpose(e, compound, name, target);
             },
             py::arg("compound"), py::arg("name"), py::arg("target"),
             "A compound's inner param `target` (\"node.param\") shown on it as its param `name` (\"\": the param's, or "
             "node + Param when taken: graph_controls.h). Returns the name.")
        .def("compound_unexpose",
             [](GraphEdit& e, const std::string& compound, const std::string& target) { return compoundUnexpose(e, compound, target); },
             py::arg("compound"), py::arg("target"),
             "The compound stops showing `target`, its value going back inside; False when it was not shown.")
        .def("copy_nodes", &GraphEdit::copyNodes, py::arg("names"),
             "The nodes (never an Output node) and the links among them as JSON text for the clipboard; \"\" for none.")
        .def("paste_nodes", &GraphEdit::pasteNodes, py::arg("clipboard"), py::arg("dx") = 0.0, py::arg("dy") = 0.0,
             "The copied nodes added under free names, moved by (dx, dy), with the links among them: [(new node, the node it "
             "copies)]. GraphError (nothing changed) for text that holds no copied nodes.")
        .def("duplicate_nodes", &GraphEdit::duplicateNodes, py::arg("names"), py::arg("dx") = 0.0, py::arg("dy") = 0.0,
             "Duplicate (Ctrl+D): the nodes copied beside them, the copies reading what the nodes read: [(copy, node)].")
        .def("output_links", &GraphEdit::outputLinks, py::arg("src"), py::arg("output") = std::string(),
             "The links reading src's output (\"\": its first): [(node, input)].")
        .def("move_output_links", &GraphEdit::moveOutputLinks, py::arg("src"), py::arg("output"), py::arg("new_src"),
             py::arg("new_output") = std::string(),
             "Every link reading src's output reads new_src's new_output instead where it fits: the (node, input) moved.")
        .def("pass_through", &GraphEdit::passThrough, py::arg("name"), "The inputs the node hands on as outputs (thru_<input>).")
        .def("set_pass_through", &GraphEdit::setPassThrough, py::arg("name"), py::arg("input"), py::arg("on"),
             "An input's pass-through output on or off (off: what read it reads nothing). False when nothing changed.")
        .def("layout_positions",
             [](const GraphEdit& e, double column, double row, int columnsPerBlock, double blockGap,
                const std::optional<std::vector<std::string>>& only) {
                 GraphLayout layout;
                 layout.column = column;
                 layout.row = row;
                 layout.columnsPerBlock = columnsPerBlock;
                 layout.blockGap = blockGap;
                 return layoutPositions(e, layout, only ? &*only : nullptr);
             },
             py::arg("column") = kInsertColumn, py::arg("row") = kInsertRow, py::arg("columns_per_block") = 0,
             py::arg("block_gap") = 60.0, py::arg("only") = py::none(),
             "The auto layout every host uses (graph_inserts.h layoutPositions): {node: (x, y)}, columns by depth from the "
             "sources, rows in the graph's order; columns_per_block wraps a long chain into blocks; only: just these nodes.")
        .def("add_control",
             [](GraphEdit& e, const std::string& node, const std::string& param, const std::string& name, std::optional<double> value,
                double min, double max, std::optional<double> groomSize) {
                 return addControl(e, node, param, name, value, min, max, groomSize);
             },
             py::arg("node"), py::arg("param"), py::arg("name") = std::string(), py::arg("default") = py::none(),
             py::arg("min") = 0.0, py::arg("max") = 1.0, py::arg("groom_size") = py::none(),
             "A Control node driving node.param (graph_controls.h addControl); returns the Control node's name.")
        .def("link_control",
             [](GraphEdit& e, const std::string& node, const std::string& param, const std::string& wanted,
                const std::vector<std::string>& taken) { return linkControl(e, node, param, wanted, taken); },
             py::arg("node"), py::arg("param"), py::arg("wanted") = std::string(), py::arg("taken") = std::vector<std::string>(),
             "Keys node.param on a control of its own (graph_controls.h linkControl); returns the control as JSON.")
        .def("unique_name", &GraphEdit::uniqueName, py::arg("base"))
        .def("add_node", &GraphEdit::addNode, py::arg("type"), py::arg("name") = std::string(),
             py::arg("params_json") = std::string(), py::arg("x") = py::none(), py::arg("y") = py::none(),
             py::arg("groom_size") = py::none(),
             "Returns the node's name (`name`, else the type's made unique). `groom_size`: the scalp's size; the "
             "defaults that measure something are fitted to it (presets' reference size 20).")
        .def("remove_node", &GraphEdit::removeNode, py::arg("name"))
        .def("rename_node", &GraphEdit::renameNode, py::arg("name"), py::arg("new_name"),
             "Renames a node; its connections and the graph's output follow.")
        .def("output_node", &GraphEdit::outputNode)
        .def("set_output_node", &GraphEdit::setOutputNode, py::arg("name"))
        .def("clear_positions", &GraphEdit::clearPositions, "Drops every node's position, for a layout afresh.")
        .def("connect", &GraphEdit::connect, py::arg("src"), py::arg("dst"), py::arg("input"), py::arg("output") = std::string())
        .def("connect_by_hand", &GraphEdit::connectByHand, py::arg("src"), py::arg("dst"), py::arg("input"),
             py::arg("output") = std::string(),
             "A link the user drew: connect, then the node set to use it (a Clump of centersFrom pick fed centers: "
             "inputPoints). True when a param changed.")
        .def("disconnect", &GraphEdit::disconnect, py::arg("dst"), py::arg("input"))
        .def(
            "detach",
            [](GraphEdit& e, const std::string& name, double drop) {
                const GraphEdit::Detached d = e.detach(name, drop);
                py::dict out;
                out["source"] = d.source;
                py::list relinked;
                for (const auto& [node, input] : d.relinked) relinked.append(py::make_tuple(node, input));
                out["relinked"] = relinked;
                out["changed"] = d.changed;
                return out;
            },
            py::arg("name"), py::arg("drop") = 90.0,
            "Detach (Ctrl+Shift+X): the node taken out of the flow, what fed it feeding what it fed, the node kept "
            "below the new link. {'source', 'relinked': [(node, input)], 'changed'}.")
        .def(
            "remove_node_keeping_flow",
            [](GraphEdit& e, const std::string& name) {
                const GraphEdit::Detached d = e.removeNodeKeepingFlow(name);
                py::dict out;
                out["source"] = d.source;
                py::list relinked;
                for (const auto& [node, input] : d.relinked) relinked.append(py::make_tuple(node, input));
                out["relinked"] = relinked;
                return out;
            },
            py::arg("name"),
            "Delete (every host's graph editor): the node removed, what fed it feeding what it fed. "
            "{'source', 'relinked': [(node, input)]}.")
        .def("check_connections", &GraphEdit::checkConnections)
        .def("node_spec_json", &GraphEdit::nodeSpecJson, py::arg("name"))
        .def("output_type", &GraphEdit::outputType, py::arg("name"), py::arg("output") = std::string())
        .def("output_types", &GraphEdit::outputTypes, py::arg("name"),
             "Each of the node's outputs' types as connected, in its spec's order.")
        .def("viewable", &GraphEdit::viewable, py::arg("name"),
             "View Up to Here: whether an output gives strands as connected (Strands, or Curves fed nothing).")
        .def("bakeable", &GraphEdit::bakeable, py::arg("name"),
             "Bake Node to Guides: not the Output node, its first output giving strands or guides as connected.")
        .def("value_kind_for_input", &GraphEdit::valueKindForInput, py::arg("src"), py::arg("dst"), py::arg("input"),
             "The kind a Value node no link leaves takes when drawn into dst's param input ('' none); "
             "connect_by_hand sets it.")
        .def("upstream", &GraphEdit::upstream, py::arg("name"))
        .def("clump_channel", &GraphEdit::clumpChannel, py::arg("node"),
             "The strand channel a Clump writes (its clumpChannel; '' for another node, or none).")
        .def("clump_color_source", &GraphEdit::clumpColorSource, py::arg("node"),
             "Clump Colors' source for this Clump: '<channel>@<node>' ('' when it writes no channel).")
        .def("clump_colored", &GraphEdit::clumpColored, py::arg("node"), py::arg("source"),
             "Whether a clump color source shows this Clump's clumps (its own source, or the bare channel when "
             "it is the last Clump writing it).")
        .def("braid_tubes_offered", &GraphEdit::braidTubesOffered, py::arg("node"),
             "Show Tubes: whether the node has the button (a Braid node).")
        .def("braid_tubes_shown", &GraphEdit::braidTubesShown, py::arg("node"), py::arg("shown"),
             "Whether the node's Show Tubes is lit: a Braid node named in `shown` (names, one a line).")
        .def("braid_tubes_toggled", &GraphEdit::braidTubesToggled, py::arg("node"), py::arg("shown"),
             "`shown` after a click on the node's Show Tubes: the node added or taken out (not a Braid: as it is).")
        .def("braid_tubes_kept", &GraphEdit::braidTubesKept, py::arg("shown"),
             "`shown` without the names that are no Braid node of the graph now (a node deleted).")
        .def("seed_warnings", &GraphEdit::seedWarnings)
        .def("feeding_output", &GraphEdit::feedingOutput,
             "The nodes the output is made from (it and everything upstream), in the graph's order: the ones the groom evaluates.")
        .def("problems", &GraphEdit::problems, py::arg("feeding_output") = true,
             "What would stop the groom from evaluating (graph_edit.h): the graph's own and those of the nodes the output is made "
             "from, or with feeding_output False those of the other nodes (warnings).")
        .def("channels", [](const GraphEdit& e, const std::vector<std::string>& guideMaps) { return jsonLoads(e.channelsJson(guideMaps)); },
             py::arg("guide_maps") = std::vector<std::string>(),
             "The strand channels (attributes) per node: graph_channels.")
        .def("complete_params", &GraphEdit::completeParams,
             "Writes every param each node leaves out (its spec default) and each node's version.")
        .def("node_version", &GraphEdit::nodeVersion, py::arg("name"))
        .def("upgrade_node", &GraphEdit::upgradeNode, py::arg("name"),
             "Raises an outdated node to its operator's newest behaviour; False when it is the newest.")
        .def("update_library_node", [](GraphEdit& e, const std::string& name, py::object size, const std::string& id) {
                 return e.updateLibraryNode(name, size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()), id);
             }, py::arg("name"), py::arg("groom_size") = py::none(), py::arg("id") = std::string(),
             "Takes the library's copy of a library node (or of library node `id`: a compound just saved as it); "
             "exposed values kept where they still are. False when it had it already. GraphError for a node that "
             "is not one or an id the library lacks.")
        .def("collapse", &GraphEdit::collapse, py::arg("names"), py::arg("compound") = std::string(),
             "Group: the nodes replaced by one Compound node holding them (every field kept), its inputs one per "
             "outside source; returns its name. GraphError (the graph unchanged) when it cannot.")
        .def("expand", &GraphEdit::expand, py::arg("compound"),
             "Ungroup: a Compound node replaced by the nodes it holds (exposed values applied, every field kept); "
             "returns their names. GraphError (the graph unchanged) when it cannot.")
        .def("moved_nodes", &GraphEdit::movedNodes,
             "The node paths this edit's renames, groups and ungroups moved, in order: [(old, new)] ('sculpt1' -> "
             "'compound1/sculpt1' for Group). What is kept by node path outside the graph follows them.");
    m.def("graph_channels", [](const std::string& graph, const std::vector<std::string>& guideMaps) { return jsonLoads(graphChannelsJson(graph, guideMaps)); },
          py::arg("graph_json"), py::arg("guide_maps") = std::vector<std::string>(),
          "The strand channels (attributes) per node of a graph's JSON, without evaluating it: {node: {'incoming': "
          "[{'name', 'node'}] (reaching it, each with the node that wrote it last), 'writes': [{'name', 'param', "
          "'extends'}], 'overwrites': [{'name', 'param', 'node'}] (a name an upstream node wrote), 'reads': "
          "[{'param', 'name', 'reaches', 'node'}]}}. guide_maps: the maps painted on the guides (Interpolate turns them "
          "into channels).");
    m.def("complete_graph_json", &completeGraphJson, py::arg("graph_json"),
          "The graph with every param and node version written (what saved grooms hold).");
    m.def("node_spec_json", [](const std::string& node) { return nodeSpecJson(node); }, py::arg("node_json"),
          "A node's ports and params (operator_specs' form) from its own JSON: a Compound's from its definition, "
          "an Input's from its type, the exposed params' ports.");

    // Graph presets (presets.h): what every host reads the preset files through. The Maya host's
    // presets.py fits a preset to a scalp size here, so the app and Maya fit the same way.
    m.def("fit_preset_json", [](const std::string& preset, py::object size) {
              return fitPresetJson(preset, size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()));
          }, py::arg("preset_json"), py::arg("size"),
          "The preset's JSON with its sizes scaled from the scalp size it records to `size` (None: unchanged); the "
          "\"size\" key dropped either way.");
    m.attr("DEFAULTS_SCALP_SIZE") = kDefaultsScalpSize;  // the scalp size the spec defaults are authored at
    m.def("default_graph_json_for", [](py::object size) {
              return defaultGraphJsonFor(size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()));
          }, py::arg("size"),
          "The default graph with its nodes' defaults fitted from the 20-unit head they are authored at to a scalp "
          "of `size` (None: as it is): what a new groom on a scalp stores.");
    m.def("preset_graph_json", [](const std::string& preset, py::object size) {
              return presetGraphJson(preset, size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()));
          }, py::arg("preset_json"), py::arg("size"),
          "The graph a groom stores from a preset: fitted, the preset's own fields dropped, its backdrops kept.");
    m.def("insert_preset_json", [](const std::string& graph, const std::string& preset, py::object size, const std::string& after,
                                   const std::string& name) {
              std::string made;
              const std::string out = insertPresetJson(graph, preset, size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()),
                                                       after, name, &made);
              return py::make_tuple(out, made);
          }, py::arg("graph_json"), py::arg("preset_json"), py::arg("size"), py::arg("after") = std::string(),
          py::arg("name") = std::string(),
          "The graph with the preset's look inserted as one Compound after node `after` (\"\": before the Output), fitted "
          "to `size`: (the graph's JSON, the Compound's name). Raises RuntimeError when it cannot be inserted.");
    m.def("preset_node_name", &presetNodeName, py::arg("name"), "The Compound name a preset's name gives: cards_mid -> cardsMid.");
    m.def("preset_fields", [] { return presetFields(); }, "A preset's own fields: size, about, requires, backdrops, controls, exposed.");
    m.def("preset_categories", [] { return presetCategories(); }, "The Presets menu's categories, in order.");
    m.def("preset_title", &presetTitle, py::arg("name"), "A preset's title when its file gives none: its name in words.");
    m.def("compound_inner_params_json", &compoundInnerParamsJson, py::arg("definition_json"),
          "A Compound's Show Inner Param list (graph_controls.h, every host's): [{node, params: [spec]}] as JSON.");
    m.def("make_preset_json", &makePresetJson, py::arg("graph_json"), py::arg("size"), py::arg("title") = std::string(),
          py::arg("description") = std::string(), py::arg("category") = std::string(), py::arg("controls_json") = std::string(),
          "The preset a groom's graph makes (Save as Preset, Copy Graph): presets.h, every host's.");
    m.def("preset_param_problems", &presetParamProblems, py::arg("graph_json"),
          "What is wrong with a graph's params against their specs (presets.h, every host's check): [] when fine.");
    m.def("preset_from_text", &presetFromText, py::arg("text"),
          "The preset a pasted text holds, every host's checks (presets.h presetFromText): its JSON; ValueError saying why.");
    m.def("pasted_graph_from_text",
          [](const std::string& text) {
              const PastedGraph pasted = pastedGraphFromText(text);
              return py::make_tuple(pasted.presetJson, pasted.removed);
          },
          py::arg("text"),
          "(preset_json, removed): a groom's whole graph pasted, every host's checks (presets.h pastedGraphFromText: "
          "the graph whole, backdrops as drawn, params naming files on other machines taken out: removed {\"node.param\": path}); "
          "ValueError saying why.");
    m.attr("MAX_PRESET_TEXT") = kMaxPresetText;
    m.attr("MAX_PRESET_NODES") = kMaxPresetNodes;
    m.def("preset_search_dirs", &presetSearchDirs, py::arg("user_dir"), py::arg("shipped_dir"),
          "Where presets are looked up, in order (presets.h): $GROOMLAB_PRESET_PATH's folders, the user's, the shipped.");
    m.def(
        "list_presets",
        [](const std::vector<std::string>& dirs) {
            py::list out;
            for (const PresetInfo& p : listPresets(dirs)) {
                py::dict d;
                d["name"] = p.name;
                d["title"] = p.title;
                d["description"] = p.description;
                d["category"] = p.category;
                d["path"] = p.path;
                out.append(d);
            }
            return out;
        },
        py::arg("dirs"), "Every preset in `dirs` (the first folder holding a name wins), sorted: [{name, title, description, category, path}].");
    m.def("preset_path", &presetPath, py::arg("name"), py::arg("dirs"), "A preset's file by name in `dirs` (\"\": none).");
    m.def(
        "preset_info",
        [](const std::string& name, const std::string& path) {
            const PresetInfo p = presetInfo(name, path);
            py::dict d;
            d["name"] = p.name;
            d["title"] = p.title;
            d["description"] = p.description;
            d["category"] = p.category;
            d["path"] = p.path;
            return d;
        },
        py::arg("name"), py::arg("path"), "What the Presets menu shows of a preset (a broken file: \"Cannot be read: ...\").");
    m.def("preset_name_error", &presetNameError, py::arg("name"), "Why a preset name cannot name a file (\"\": it can).");
    m.def(
        "preset_required_maps",
        [](const std::string& presetJson) {
            py::list out;
            for (const PresetMap& m : presetRequiredMaps(presetJson)) {
                py::dict d;
                d["name"] = m.name;
                d["kind"] = m.kind;
                d["value"] = m.value;
                d["color"] = m.color.empty() ? py::object(py::none()) : py::object(py::cast(m.color));
                out.append(d);
            }
            return out;
        },
        py::arg("preset_json"), "The painted maps a preset's graph reads: [{name, kind, value, color}] (presets.h).");
    m.def(
        "preset_required_groups",
        [](const std::string& presetJson) {
            py::list out;
            for (const PresetGroup& g : presetRequiredGroups(presetJson)) {
                py::dict d;
                d["name"] = g.name;
                d["draw"] = g.draw;
                out.append(d);
            }
            return out;
        },
        py::arg("preset_json"), "The guide groups a preset's graph reads: [{name, draw}] (presets.h).");
    m.def("preset_user_dir", &presetUserDir, "The user's preset folder (GROOMLAB_USER_PRESETS, else Maya's user folder).");

    m.def("braid_sections",
          [](const std::string& style, int sections, bool dutch, float flatness, float pancake, float tightness, float separate,
             float inflate, int version, int perStep, int around) {
              BraidWeaveSettings settings;
              settings.style = style;
              settings.sections = sections;
              settings.dutch = dutch;
              settings.flatness = flatness;
              settings.pancake = pancake;
              settings.tightness = tightness;
              settings.separate = separate;
              settings.inflate = inflate;
              settings.version = version;
              const BraidSections made = braidSections(settings, perStep, around);
              auto pairs = [](const std::vector<std::vector<std::array<float, 2>>>& from) {
                  py::list out;
                  for (const auto& section : from) {
                      py::list track;
                      for (const auto& v : section) track.append(py::make_tuple(v[0], v[1]));
                      out.append(track);
                  }
                  return out;
              };
              // Each section's outline per sample: [section][sample] a list of (side, depth) points.
              py::list outlines;
              double least = std::numeric_limits<double>::infinity(), most = 0.0, sum = 0.0;
              for (int k = 0; k < made.sections; ++k) {
                  py::list track;
                  for (std::size_t i = 0; i < made.samples(); ++i) {
                      py::list ring;
                      const std::array<float, 2>* p = made.outline(k, i);
                      for (int r = 0; r < made.around; ++r) ring.append(py::make_tuple(p[r][0], p[r][1]));
                      track.append(ring);
                      const double area = made.area(k, i);
                      least = std::min(least, area);
                      most = std::max(most, area);
                      sum += area;
                  }
                  outlines.append(track);
              }
              const std::array<float, 2> reach = made.reach();
              py::dict out;
              out["sections"] = made.sections;
              out["cycle"] = made.cycle;
              out["perStep"] = made.perStep;
              out["around"] = made.around;
              out["round"] = made.round;
              out["pressed"] = made.pressed;
              out["halfWidth"] = made.halfWidth;
              out["halfThick"] = made.halfThick;
              out["centers"] = pairs(made.centers);
              out["across"] = pairs(made.across);
              out["thick"] = pairs(made.thick);
              out["outlines"] = outlines;
              out["keep"] = py::make_tuple(made.keep[0], made.keep[1]);
              out["grown"] = made.grown;
              out["overlap"] = made.overlap();
              out["reach"] = py::make_tuple(reach[0], reach[1]);
              const double count = static_cast<double>(made.sections) * static_cast<double>(made.samples());
              out["areas"] = py::make_tuple(count > 0.0 ? least : 0.0, count > 0.0 ? sum / count : 0.0, most);
              return out;
          },
          py::arg("style") = std::string("three"), py::arg("sections") = 0, py::arg("dutch") = false, py::arg("flatness") = 0.3f,
          py::arg("pancake") = 0.0f, py::arg("tightness") = 1.0f, py::arg("separate") = 1.0f, py::arg("inflate") = 0.5f,
          py::arg("version") = 2, py::arg("per_step") = 16, py::arg("around") = 48,
          "One cycle of a Braid's weave in the braid's cross-section (braid_tubes.h), in braid widths ([0] side to side, "
          "[1] depth), as a node of this version lays it (1: plain ellipses; 2: pressed against each other): {sections, "
          "cycle, perStep, around, round, pressed, halfWidth, halfThick, centers, across, thick (each [section][sample]: "
          "the center and the ellipse's two half axes), outlines ([section][sample]: `around` points, what to draw), keep "
          "(the cross-section's scale that keeps the braid's reach), grown, overlap (the deepest one outline lies in "
          "another), reach (side, depth), areas (least, mean, most)}.");

    // Compound nodes (node_library.h): the .glnode library every host shares.
    m.def("check_node_file", &checkNodeFile, py::arg("text"),
          "A node file's text checked and made a Compound's definition (JSON text); GraphError says what is wrong.");
    m.def("node_id_error", &nodeIdError, py::arg("id"), "Why an id cannot be a node's (\"\" when it can).");
    m.def("suggest_node_id", &suggestNodeId, py::arg("label"), py::arg("author") = std::string(),
          "An id for a new node: author.label as lowercase words joined by _.");
    m.def("node_user_dir", &nodeUserDir, "The user's node folder (GROOMLAB_USER_NODES, else Documents/GroomLab/nodes).");
    m.def("set_shipped_node_dir", &setShippedNodeDir, py::arg("dir"), "The folder of nodes shipped with this host.");
    m.def("shipped_node_dir", &shippedNodeDir);
    m.def("refresh_node_library", &refreshNodeLibrary, "Reads the node folders again at the next look.");
    m.def("node_search_dirs", &nodeSearchDirs, "GROOMLAB_NODE_PATH's folders, the user's folder, the shipped folder.");
    m.def("library_nodes", [] { return jsonLoads(libraryNodesJson()); },
          "The library: {'nodes': [Add Node entries, type 'node:<id>'], 'errors': [{'path', 'id', 'error'}], 'dirs', 'userDir'}.");
    m.def("node_file_from_compound", [](const std::string& graph, const std::string& compound, const std::string& header, py::object size) {
              return nodeFileFromCompound(graph, compound, header, size.is_none() ? std::nullopt : std::optional<double>(size.cast<double>()));
          }, py::arg("graph_json"), py::arg("compound"), py::arg("header_json") = std::string(), py::arg("size") = py::none(),
          "The node file (JSON text) made from a Compound of the graph; header: id, version, label, category, "
          "description, author, license, icon, params (names), paramInfo.");
    m.def("save_node_file", &saveNodeFile, py::arg("text"), py::arg("dir") = std::string(),
          "Writes a node file (checked) as <id>.glnode in dir (\"\": the user's folder); returns its path.");
    m.def("import_node_file", &importNodeFile, py::arg("path"), py::arg("dir") = std::string(),
          "Copies a node file (checked) into dir (\"\": the user's folder); returns the new path.");
    // Script nodes (script.h, node_library.h).
    m.def("is_script_node_file", &isScriptNodeFile, py::arg("text"), "Whether a node file (or definition) is a script node's.");
    m.def("script_node_errors", [](const std::string& text) {
              const py::object all = jsonLoads(scriptNodeErrorsJson(text));
              return py::object(all["errors"]);  // (an accessor would outlive the dict it reads)
          },
          py::arg("text"),
          "What is wrong with a script node file: [{'part': 'json'|'header'|'script', 'message', 'line', 'column'}] "
          "(line and column in the script; 0 for the header's); [] when it checks.");
    m.def("script_node_template", &scriptNodeTemplate, py::arg("label") = std::string("My Script"), py::arg("author") = std::string(),
          "A new script node file (JSON text) to start from.");
    m.def("library_node_name", &libraryNodeName, py::arg("label"), "The name a node added from the library takes: \"Soft Frizz\" -> \"softFrizz\".");
    m.def("script_builtin_names", &scriptBuiltinNames, "The names a script reads as built-in (no param may take one).");
    m.def("script_functions", [] {
              py::list out;
              for (const ExpressionFunction& f : scriptFunctions()) {
                  py::dict d;
                  d["name"] = f.name;
                  d["arguments"] = f.arguments;
                  d["about"] = f.about;
                  out.append(d);
              }
              return out;
          },
          "The functions a script has beyond an expression's: [{'name', 'arguments', 'about'}].");
    m.attr("SCRIPT_STEP_BUDGET") = kScriptStepBudget;

    m.def("starter_look_given", &starterLookGiven, py::arg("graph_json"), py::arg("look_types"),
          "The first-groom card's look step (presets.h): a look node beyond the default graph's own.");
    m.def("with_script_definition", &withScriptDefinition, py::arg("graph_json"), py::arg("node"), py::arg("definition_json"),
          "The graph with script node `node` given an edited definition (node_library.h: every host's Edit Script); "
          "ValueError listing what does not check.");
    m.def("with_imported_curves", &withImportedCurves, py::arg("graph_json"), py::arg("path"), py::arg("object") = std::string(),
          py::arg("samples") = 24,
          "The graph with its guides node an ImportCurves node reading `path` (graph_inserts.h: every host's Import Alembic Curves).");
    m.def("group_readers", &groupReaders, py::arg("graph_json"), py::arg("group"),
          "[(node, param)] of the Guides nodes whose group or exclude names guide group `group` (graph_inserts.h).");
    m.def("rename_group_readers",
          [](const std::string& graphJson, const std::string& from, const std::string& to) {
              bool changed = false;
              std::string text = renameGroupReaders(graphJson, from, to, &changed);
              return py::make_tuple(text, changed);
          },
          py::arg("graph_json"), py::arg("old"), py::arg("new"),
          "(graph_json, changed): every Guides node's group and exclude naming group `old` naming `new`, inside compounds too.");

    // Built-in operator type names, for scripts.
    py::dict ops;
    for (const std::string& name : OperatorRegistry::instance().typeNames()) ops[py::str(name)] = name;
    m.attr("OPERATORS") = ops;
}

}  // namespace groomlab::py_bind
