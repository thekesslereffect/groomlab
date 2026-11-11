// SPDX-License-Identifier: Apache-2.0
// The operator graph: ports, parameters, operators and the registry,
// external inputs, the evaluation context, Graph and GraphDocument.

#include "bind_common.h"

#include <sstream>

#include "groomlab/attributes.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
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
    m.def("controls_json", &controlsJson, py::arg("graph_json"),
          "Every control the graph reads: {name: {kind, node, default, ...}} as JSON (graph_controls.h).");

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
        .def("insert_guide_cache",
             [](GraphEdit& e, const std::string& path, const std::string& object, std::optional<double> groomSize) {
                 return insertGuideCache(e, path, object, groomSize);
             },
             py::arg("path"), py::arg("object") = std::string(), py::arg("groom_size") = py::none(),
             "Load Guide Cache (graph_inserts.h): a GuideCache node right after the Guides node, or the one there reading the file.")
        .def("insert_collider",
             [](GraphEdit& e, const std::string& mesh, bool wire, std::optional<double> offset, std::optional<double> friction,
                std::optional<double> groomSize) { return insertCollider(e, mesh, wire, offset, friction, groomSize); },
             py::arg("mesh_name"), py::arg("wire") = true, py::arg("offset") = py::none(), py::arg("friction") = py::none(),
             py::arg("groom_size") = py::none(),
             "Add > Collider's graph (graph_inserts.h): a ColliderMesh node, and with `wire` a Collide before the output.")
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
             "Every link from src into dst through `node` (graph_inserts.h).")
        .def("not_keyable", [](const GraphEdit& e, const std::string& node, const std::string& param) { return notKeyable(e, node, param); },
             py::arg("node"), py::arg("param"), "Why node.param cannot be keyed in place (\"\" when it can).")
        .def("expose_param", [](GraphEdit& e, const std::string& node, const std::string& param) { exposeParam(e, node, param); },
             py::arg("node"), py::arg("param"), "node.param as an input port (graph_controls.h).")
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
        .def("check_connections", &GraphEdit::checkConnections)
        .def("node_spec_json", &GraphEdit::nodeSpecJson, py::arg("name"))
        .def("output_type", &GraphEdit::outputType, py::arg("name"), py::arg("output") = std::string())
        .def("upstream", &GraphEdit::upstream, py::arg("name"))
        .def("seed_warnings", &GraphEdit::seedWarnings)
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
    m.def("preset_from_text", &presetFromText, py::arg("text"),
          "The preset a pasted text holds, every host's checks (presets.h presetFromText): its JSON; ValueError saying why.");
    m.attr("MAX_PRESET_TEXT") = kMaxPresetText;
    m.attr("MAX_PRESET_NODES") = kMaxPresetNodes;
    m.def("preset_user_dir", &presetUserDir, "The user's preset folder (GROOMLAB_USER_PRESETS, else Maya's user folder).");

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

    // Built-in operator type names, for scripts.
    py::dict ops;
    for (const std::string& name : OperatorRegistry::instance().typeNames()) ops[py::str(name)] = name;
    m.attr("OPERATORS") = ops;
}

}  // namespace groomlab::py_bind
