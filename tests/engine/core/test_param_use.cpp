// SPDX-License-Identifier: Apache-2.0
// Dead params: every param an operator declares does something in some state where a UI shows it
// (its showWhen holds). Each operator is evaluated with a ParamSet that records the names it reads
// (ParamReadLog), on the inputs its nodes get in the shipped presets (and the test's own grooms) on
// the reference groom, and, for an operator no preset uses, on values of each port's type those
// grooms make. Each is run in many states: its params as the preset sets them and with every number
// at 0 set to something else, each value of each choice, each bool flipped, each optional input
// opened or linked. A param never read in any state where its showWhen holds fails, unless the
// allowlist below says why (read by hosts, the renderer or the graph, not the operator).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/paths.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "groomlab/presets.h"
#include "groomlab/simulation.h"
#include "groomlab/strand_geometry.h"

namespace groomlab {
namespace {

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;
constexpr float kDensity = 0.05f;
constexpr std::size_t kInstancesPerType = 2;  // preset nodes of one type evaluated in every state

// Params an operator never reads itself, each with who does.
const std::map<std::string, std::string>& allowlist() {
    static const std::map<std::string, std::string> list = {
        // The render look: GroomSession::hairLook reads them off the groom's FeatherColor node for
        // the shaders (Maya's, Arnold's, Blender's), not the strands.
        {"FeatherColor.iridescenceColor", "render look (GroomSession::hairLook)"},
        {"FeatherColor.iridescenceShift", "render look (GroomSession::hairLook)"},
        // The slider range of the control a host makes for the node (Maya's ctl_ attribute).
        {"Control.min", "the host's control slider (hosts/maya/scripts/groomlab/params.py)"},
        {"Control.max", "the host's control slider (hosts/maya/scripts/groomlab/params.py)"},
        // The solver's friction against this mesh: GroomSession::simInputs reads it when the mesh
        // feeds a Simulate node's collider.
        {"ColliderMesh.friction", "the session's simulation (GroomSession::simInputs)"},
        // Read in viewport evaluations only (these states evaluate as renders do): test_cards.cpp's
        // Subdivide tests and the GPU preview's (look_chain.cpp).
        {"Subdivide.viewportLevels", "viewport evaluations (EvalContext::viewport)"},
    };
    return list;
}

// Operators no state here can evaluate, each with why and what tests it instead.
const std::map<std::string, std::string>& untestable() {
    static const std::map<std::string, std::string> list = {
        {"Map", "reads a painted map by name, which the reference groom has none of (test_maps.cpp)"},
    };
    return list;
}

// Text params a state sets to a value the fixture holds (an empty one is refused: shown on the node).
const std::map<std::string, ParamValue>& fixtureValues() {
    static const std::map<std::string, ParamValue> list = {
        {"Group.guideGroup", std::string("primary")},  // the reference hierarchy's top group
        {"Plume.rankChannel", std::string("featherId")},
    };
    return list;
}

// One node as a preset evaluated it: its params, version, path and the values at its inputs.
struct Instance {
    std::string where;  // "preset/node"
    ParamSet params;
    int version = 1;
    std::string path;
    std::vector<Value> inputs;  // the operator's own inputs (exposed params' ports left out)
};

struct Fixtures {
    GroomFile reference;
    std::map<std::string, std::vector<Instance>> byType;
    std::map<PortType, std::vector<Value>> pool;  // values each preset node made, by output type
    std::shared_ptr<const PathResolver> paths;
};

bool fits(PortType port, const Value& v) {
    switch (port) {
    case PortType::Strands: return valueAs<StrandsPtr>(v) != nullptr;
    case PortType::Guides: return valueAs<GuidesPtr>(v) != nullptr;
    case PortType::Mesh: return valueAs<MeshPtr>(v) != nullptr;
    case PortType::Field: return valueAs<FieldPtr>(v) != nullptr;
    case PortType::Curve: return valueAs<CurvePtr>(v) != nullptr;
    case PortType::Curves: return valueAs<StrandsPtr>(v) != nullptr || valueAs<GuidesPtr>(v) != nullptr;
    }
    return false;
}

const Fixtures& fixtures() {
    static const Fixtures f = [] {
        Fixtures out{readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom"), {}, {}, nullptr};
        out.paths = std::make_shared<const PathResolver>(searchPathResolver({out.reference.directory, kShipped}));
        const std::optional<double> size = presetScalpSize(*out.reference.data.scalp);
        for (const PresetInfo& p : listPresets({kShipped, std::string(GROOMLAB_TEST_DATA) + "/presets"})) {
            std::ifstream in(std::filesystem::u8path(p.path), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            GroomData data = out.reference.data;
            data.graphJson = presetGraphJson(text.str(), size);
            try {
                GroomSession groom(data);
                groom.setPathResolver(out.paths);
                groom.evaluate(kDensity, 0.0);
                const GraphDocument* doc = groom.viewDocument();
                if (!doc) continue;
                // The preset's own nodes, by name: its compounds' nodes (held inline by their
                // paths) are the compounds' business, not fixtures here; what a compound gives is
                // pooled, under the compound's name.
                std::map<std::string, std::pair<std::string, NodeId>> nodes;  // name as the graph has it -> (name, node)
                for (const auto& [name, id] : doc->nodesByName) {
                    if (name.find('/') == std::string::npos) nodes[name] = {name, id};
                }
                for (const auto& [compound, inner] : groom.flatCompounds().output) {
                    if (compound.find('/') == std::string::npos && doc->nodesByName.count(groom.flatCompounds().result(compound))) {
                        const std::string result = groom.flatCompounds().result(compound);
                        nodes[compound] = {result, doc->nodesByName.at(result)};
                    }
                }
                for (const auto& [shown, at] : nodes) {
                    const auto& [name, id] = at;
                    const bool inner = name.find('/') != std::string::npos;
                    const Operator& op = doc->graph.node(id);
                    const std::vector<PortSpec> outs = op.outputs();
                    for (std::size_t o = 0; o < outs.size(); ++o) {
                        const Value v = doc->graph.cachedValue(id, o);
                        if (v.index() != 0) out.pool[outs[o].type].push_back(v);
                    }
                    if (inner) continue;
                    std::vector<Instance>& list = out.byType[op.typeName()];
                    if (list.size() >= kInstancesPerType) continue;
                    Instance inst{p.name + "/" + name, doc->graph.storedParams(id), op.nodeVersion(), op.nodePath(), {}};
                    bool complete = true;
                    for (std::size_t slot = 0; slot < op.inputs().size(); ++slot) {
                        NodeId src = 0;
                        std::size_t output = 0;
                        if (!doc->graph.inputSource(id, slot, &src, &output)) {
                            inst.inputs.emplace_back();
                            continue;
                        }
                        inst.inputs.push_back(doc->graph.cachedValue(src, output));
                        complete = complete && inst.inputs.back().index() != 0;
                    }
                    if (complete) list.push_back(std::move(inst));
                }
                // The compounds, which the session holds as their nodes inline: each as the
                // graph stores it, taking what feeds it there (the graph's own nodes keep their names).
                const GraphDocument stored = loadGraphJson(data.graphJson);
                for (const auto& [name, id] : stored.nodesByName) {
                    const Operator& op = stored.graph.node(id);
                    if (op.typeName() != op::kCompound) continue;
                    std::vector<Instance>& list = out.byType[op.typeName()];
                    if (list.size() >= kInstancesPerType) continue;
                    Instance inst{p.name + "/" + name, stored.graph.storedParams(id), op.nodeVersion(), op.nodePath(), {}};
                    bool complete = true;
                    for (std::size_t slot = 0; slot < op.inputs().size(); ++slot) {
                        NodeId src = 0;
                        std::size_t output = 0;
                        if (!stored.graph.inputSource(id, slot, &src, &output)) {
                            inst.inputs.emplace_back();
                            continue;
                        }
                        const auto from = doc->nodesByName.find(groom.flatCompounds().result(stored.nameOf(src)));
                        inst.inputs.push_back(from == doc->nodesByName.end() ? Value{} : doc->graph.cachedValue(from->second, output));
                        complete = complete && inst.inputs.back().index() != 0;
                    }
                    if (complete) list.push_back(std::move(inst));
                }
            } catch (const std::exception&) {
                // PresetLooks says a preset that fails; here it only gives no fixtures.
            }
        }
        return out;
    }();
    return f;
}

// A param's value as showWhen compares it (graph.h ParamCondition).
std::string conditionText(const ParamValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    if (const std::int64_t* i = std::get_if<std::int64_t>(&v)) return std::to_string(*i);
    if (const double* d = std::get_if<double>(&v)) return nlohmann::json(*d).dump();
    return {};
}

struct State {
    ParamSet params;
    std::vector<bool> linked;
};

// The condition holds, and the param it names shows too, and so on up (as UIs show it).
bool holds(const ParamCondition& condition, const State& s, const std::vector<ParamSpec>& specs) {
    const ParamCondition* c = &condition;
    for (std::size_t hops = 0; c && !c->param.empty() && hops <= specs.size(); ++hops) {
        if (!s.params.has(c->param)) return false;
        const std::string text = conditionText(s.params.get(c->param));
        if (std::find(c->in.begin(), c->in.end(), text) == c->in.end()) return false;
        const auto on = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& p) { return p.name == c->param; });
        c = on == specs.end() ? nullptr : &on->showWhen;
    }
    return true;
}

// A number other than 0 within the spec's limits.
ParamValue nonZero(const ParamSpec& spec, const ParamValue& v) {
    double value = 0.5;
    if (spec.max && *spec.max <= 0.0) value = -0.5;
    if (spec.softMax && *spec.softMax > 0.0) value = std::min(value, *spec.softMax * 0.5);
    if (spec.min) value = std::max(value, *spec.min);
    if (spec.max) value = std::min(value, *spec.max);
    if (std::holds_alternative<std::int64_t>(v)) {
        std::int64_t i = value < 0 ? -1 : 1;
        if (spec.min) i = std::max<std::int64_t>(i, static_cast<std::int64_t>(std::ceil(*spec.min)));
        if (spec.max) i = std::min<std::int64_t>(i, static_cast<std::int64_t>(std::floor(*spec.max)));
        return i;
    }
    return value;
}

bool isZero(const ParamValue& v) {
    if (const double* d = std::get_if<double>(&v)) return *d == 0.0;
    if (const std::int64_t* i = std::get_if<std::int64_t>(&v)) return *i == 0;
    return false;
}

TEST(ParamUse, EveryDeclaredParamIsReadWhereItShows) {
    const Fixtures& fx = fixtures();
    const GroomData& data = fx.reference.data;
    // The reference groom's scalp and guide hierarchy, a part across its top, the scalp as a
    // collider mesh, and the scalp moving over a few earlier frames (Jiggle's history).
    auto hierarchy = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(data.hierarchyJson));
    hierarchy->evaluate(data.scalp.get());
    PartSet partSet;
    partSet.add("middle", {{-0.5f, 0.85f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.85f, 0.0f}}, *data.scalp);
    auto parts = std::make_shared<const PartLines>(partSet, *data.scalp);
    ExternalInputs externals;
    setGroomExternals(externals, data.scalp, data.guides, hierarchy, parts);
    setMeshExternals(externals, {{"collider", data.scalp}});
    setControlExternals(externals, data.controls);
    setMapExternals(externals, data.maps);
    std::vector<std::vector<Vec3f>> frames;
    for (int f = 3; f >= 1; --f) {
        std::vector<Vec3f> moved = data.scalp->points();
        for (Vec3f& p : moved) p.x += 0.02f * static_cast<float>(f);
        frames.push_back(std::move(moved));
    }
    setScalpHistoryExternal(externals, ScalpHistory::make(1.0 / 24.0, std::move(frames)));
    EvalContext ctx;
    ctx.previewDensity = kDensity;
    ctx.externals = &externals;
    ctx.paths = fx.paths.get();
    EvalContext cardsCtx = ctx;
    cardsCtx.buildCards = true;

    std::vector<std::string> dead, untested, shownDead;
    const OperatorRegistry& registry = OperatorRegistry::instance();
    for (const std::string& type : registry.typeNames()) {
        if (type.rfind("Test", 0) == 0) continue;  // a test's own operator
        const std::unique_ptr<Operator> proto = registry.create(type);
        const std::vector<PortSpec> ports = proto->inputs();
        const std::vector<ParamSpec> specs = proto->paramSpecs();
        if (specs.empty()) continue;

        // The instances: the preset nodes of this type, else the pool's values at its inputs.
        std::vector<Instance> instances;
        auto it = fx.byType.find(type);
        if (it != fx.byType.end()) instances = it->second;
        if (instances.empty()) {
            for (std::size_t pick = 0; pick < 4; ++pick) {
                Instance inst{"pool#" + std::to_string(pick), proto->params(), proto->version(), type, {}};
                bool complete = true;
                for (const PortSpec& port : ports) {
                    std::vector<Value> fitting;
                    for (const auto& [t, values] : fx.pool) {
                        for (const Value& v : values) {
                            if (fits(port.type, v)) fitting.push_back(v);
                        }
                    }
                    if (fitting.empty()) {
                        inst.inputs.emplace_back();
                        complete = complete && port.optional;
                    } else {
                        inst.inputs.push_back(fitting[(pick * 7919) % fitting.size()]);
                    }
                }
                if (complete) instances.push_back(std::move(inst));
            }
        }

        std::map<std::string, bool> covered;
        std::vector<std::pair<State, std::set<std::string>>> runs;  // each state and what it read
        std::size_t ran = 0;
        for (const Instance& inst : instances) {
            // The input states: as the preset links them, then each optional input flipped.
            std::vector<std::vector<Value>> inputStates = {inst.inputs};
            for (std::size_t slot = 0; slot < ports.size(); ++slot) {
                if (!ports[slot].optional) continue;
                std::vector<Value> flipped = inst.inputs;
                if (flipped[slot].index() != 0) {
                    flipped[slot] = Value{};
                } else {
                    for (const auto& [t, values] : fx.pool) {
                        for (const Value& v : values) {
                            if (flipped[slot].index() == 0 && fits(ports[slot].type, v)) flipped[slot] = v;
                        }
                    }
                    if (flipped[slot].index() == 0) continue;
                }
                inputStates.push_back(std::move(flipped));
            }
            // The param states: as set, and with every number at 0 set to something else; each
            // with each choice's values and each bool flipped.
            ParamSet allOn = inst.params;
            for (const ParamSpec& spec : specs) {
                if (allOn.has(spec.name) && isZero(allOn.get(spec.name))) allOn.set(spec.name, nonZero(spec, allOn.get(spec.name)));
                const auto fixed = fixtureValues().find(type + "." + spec.name);
                if (fixed != fixtureValues().end()) allOn.set(spec.name, fixed->second);
            }
            std::vector<ParamSet> paramStates;
            for (const ParamSet& base : {inst.params, allOn}) {
                paramStates.push_back(base);
                for (const ParamSpec& spec : specs) {
                    for (const std::string& choice : spec.choices) {
                        ParamSet s = base;
                        s.set(spec.name, choice);
                        paramStates.push_back(std::move(s));
                    }
                    if (const bool* b = std::get_if<bool>(&spec.defaultValue)) {
                        ParamSet s = base;
                        s.set(spec.name, !s.getBool(spec.name, *b));
                        paramStates.push_back(std::move(s));
                    }
                }
            }
            for (const std::vector<Value>& inputs : inputStates) {
                for (const ParamSet& params : paramStates) {
                    const std::unique_ptr<Operator> op = registry.create(type);
                    op->params() = params;
                    op->setNodeVersion(inst.version);
                    op->setNodePath(inst.path);
                    auto log = std::make_shared<ParamReadLog>();
                    op->params().setReadLog(log);
                    try {
                        (void)op->externalInputs();
                        (void)op->inputs();
                        (void)op->outputs();
                        (void)op->cacheSalt();
                        for (std::size_t slot = 0; slot < inputs.size(); ++slot) (void)op->inputActive(slot);
                        // Cards build their cards only when asked (EvalContext::buildCards).
                        for (const EvalContext* c : {&ctx, &cardsCtx}) {
                            if (c == &cardsCtx && op->contextSalt(ctx) == op->contextSalt(cardsCtx)) continue;
                            (void)op->evaluateOutputs(*c, inputs);
                            (void)op->note(*c);
                        }
                        // StrandMesh's settings are read by the session and the hosts making its mesh.
                        if (type == op::kStrandMesh) (void)strandMeshSettings(op->params());
                        // Simulate's settings are read by the session's solver (simulation.h).
                        if (type == op::kSimulate) {
                            (void)SimSettings::fromParams(op->params());
                            if (const GuidesPtr guides = valueAs<GuidesPtr>(inputs.at(0))) {
                                (void)simulationMaps(op->params(), *guides, data.scalp);
                            }
                        }
                    } catch (const std::exception&) {
                        continue;  // a state the operator refuses: shown on the node, reads nothing
                    }
                    ++ran;
                    State state{params, {}};
                    for (const Value& v : inputs) state.linked.push_back(v.index() != 0);
                    const std::set<std::string> read = log->names();
                    for (const ParamSpec& spec : specs) {
                        if (read.count(spec.name) && holds(spec.showWhen, state, specs)) covered[spec.name] = true;
                    }
                    runs.emplace_back(std::move(state), read);
                }
            }
        }
        if (ran == 0) {
            if (!untestable().count(type)) untested.push_back(type);
            continue;
        }
        EXPECT_FALSE(untestable().count(type)) << type << " evaluates here: drop it from the untestable list";
        // Shown but dead: a param read in some state, never in the states (where it shows) with
        // a choice at one value, a bool at one value, or an optional input open or linked.
        for (const ParamSpec& spec : specs) {
            if (!covered.count(spec.name)) continue;
            std::map<std::string, std::pair<int, int>> byGate;  // "C=v": states shown, states read
            for (const auto& [state, read] : runs) {
                if (!holds(spec.showWhen, state, specs)) continue;
                for (const ParamSpec& gate : specs) {
                    if (gate.name == spec.name) continue;
                    if (gate.choices.empty() && !std::holds_alternative<bool>(gate.defaultValue)) continue;
                    if (!state.params.has(gate.name)) continue;
                    auto& counts = byGate[gate.name + "=" + conditionText(state.params.get(gate.name))];
                    ++counts.first;
                    counts.second += read.count(spec.name) ? 1 : 0;
                }
                for (std::size_t slot = 0; slot < ports.size(); ++slot) {
                    if (!ports[slot].optional) continue;
                    auto& counts = byGate[ports[slot].name + (state.linked[slot] ? " linked" : " open")];
                    ++counts.first;
                    counts.second += read.count(spec.name) ? 1 : 0;
                }
            }
            for (const auto& [gate, counts] : byGate) {
                if (counts.first > 0 && counts.second == 0) shownDead.push_back(type + "." + spec.name + " while " + gate);
            }
        }
        for (const ParamSpec& spec : specs) {
            const std::string name = type + "." + spec.name;
            if (!covered.count(spec.name) && !allowlist().count(name)) dead.push_back(name);
            if (covered.count(spec.name) && allowlist().count(name)) {
                ADD_FAILURE() << name << " is read by its operator: drop it from the allowlist";
            }
        }
    }
    std::string list;
    for (const std::string& d : dead) list += "\n  " + d;
    EXPECT_TRUE(dead.empty()) << dead.size() << " params never read where they show:" << list;
    std::string shown;
    for (const std::string& d : shownDead) shown += "\n  " + d;
    if (!shownDead.empty()) std::cout << shownDead.size() << " params shown where they do nothing:" << shown << "\n";
    std::string none;
    for (const std::string& t : untested) none += " " + t;
    EXPECT_TRUE(untested.empty()) << "operators no state evaluated:" << none;
}

}  // namespace
}  // namespace groomlab
