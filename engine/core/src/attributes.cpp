// SPDX-License-Identifier: Apache-2.0
#include "groomlab/attributes.h"

#include <functional>
#include <memory>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/param_conditions.h"
#include "groomlab/operators.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

constexpr int kMaxDepth = 16;  // compounds inside compounds

// Who wrote a channel last: the node's name in the graph at `depth` (0 the top graph; a compound's
// inner graph one deeper). A compound hands on what its inner nodes wrote under its own name.
struct Source {
    std::string node;
    int depth = 0;
};
using Channels = std::map<std::string, Source>;  // by channel name

json definitionOf(const json& params) {
    json d = params.is_object() ? params.value("definition", json::object()) : json::object();
    if (d.is_string()) d = json::parse(d.get<std::string>(), nullptr, false);
    return d.is_object() ? d : json::object();
}

// A param's value as ParamCondition compares it: a bool as "true"/"false", a number as JSON writes it.
std::string conditionText(const ParamValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (const std::string* s = std::get_if<std::string>(&value)) return *s;
    if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
    if (const double* d = std::get_if<double>(&value)) return groomlab::conditionText(json(*d));  // (3.0: "3")
    return {};
}

// Whether a UI shows the param as its node is set (its showWhen, and so on up).
bool shown(const Operator& op, const std::vector<ParamSpec>& specs, const ParamSpec& spec, int depth = 0) {
    if (spec.showWhen.empty()) return true;
    if (depth > 8) return true;
    const ParamSpec* other = nullptr;
    for (const ParamSpec& s : specs) {
        if (s.name == spec.showWhen.param) other = &s;
    }
    if (!other) return true;
    const std::string text = op.params().has(other->name) ? conditionText(op.params().get(other->name)) : conditionText(other->defaultValue);
    bool holds = false;
    for (const std::string& v : spec.showWhen.in) holds = holds || v == text;
    return holds && shown(op, specs, *other, depth + 1);
}

class Analysis {
public:
    Analysis(const json& graph, const std::vector<std::string>& guideMaps, std::map<std::string, Channels> inputs, int depth,
             std::map<std::string, NodeChannels>* results)
        : guideMaps_(guideMaps), inputs_(std::move(inputs)), depth_(depth), results_(results) {
        const json nodes = graph.value("nodes", json::array());
        if (nodes.is_array()) {
            for (const json& n : nodes) {
                if (n.is_object() && n.contains("name") && n["name"].is_string()) nodes_[n["name"].get<std::string>()] = n;
            }
        }
        const json links = graph.value("connections", json::array());
        if (links.is_array()) {
            for (const json& c : links) {
                if (!c.is_object()) continue;
                const std::string to = c.value("to", std::string()), input = c.value("input", std::string());
                if (!to.empty() && !input.empty()) feeds_[to][input] = c.value("from", std::string());
            }
        }
        output_ = graph.value("output", std::string());
    }

    void run() {
        for (const auto& [name, node] : nodes_) out(name);
    }
    // The channels on the graph's output node's result (empty: none).
    Channels outputChannels() { return nodes_.count(output_) ? out(output_) : Channels{}; }
    // Every inner overwrite, for the compound holding this graph (param "node.param").
    const std::vector<ChannelOverwrite>& innerOverwrites() const { return innerOverwrites_; }

private:
    const Channels& out(const std::string& name) {
        if (const auto done = out_.find(name); done != out_.end()) return done->second;
        if (!busy_.insert(name).second) {  // a cycle: nothing through it
            static const Channels none;
            return none;
        }
        Channels made = evaluate(name);
        busy_.erase(name);
        return out_[name] = std::move(made);
    }

    Channels evaluate(const std::string& name) {
        const json& node = nodes_.at(name);
        const std::string type = node.value("type", std::string());
        const json params = node.value("params", json::object());
        const bool bypassed = node.value("bypass", false);
        NodeChannels info;

        std::unique_ptr<Operator> op;
        try {
            op = operatorFromJson(type, params.is_object() ? params.dump() : std::string());
        } catch (const std::exception&) {
            // Unknown: brings nothing.
        }
        // Its inputs in slot order, and which hand their channels on.
        std::vector<std::string> inputs;
        std::vector<bool> carries;
        json definition;
        if (type == op::kCompound) {
            definition = definitionOf(params);
            for (const json& port : definition.value("inputs", json::array())) {
                const std::string t = port.value("type", std::string("Strands"));
                inputs.push_back(port.value("name", std::string()));
                carries.push_back(t == "Strands" || t == "Curves" || t == "Guides");
            }
        } else if (op) {
            const std::vector<PortSpec> ports = op->inputs();
            for (std::size_t slot = 0; slot < ports.size(); ++slot) {
                inputs.push_back(ports[slot].name);
                carries.push_back(op->carriesChannels(slot));
            }
        }
        if (bypassed) {
            for (std::size_t slot = 0; slot < carries.size(); ++slot) carries[slot] = slot == 0;
        }

        // What reaches it: each carrying input's channels, the first input's first.
        Channels incoming;
        std::map<std::string, Channels> byInput;  // a compound's inner Input nodes take these
        const auto fed = feeds_.find(name);
        for (std::size_t slot = 0; slot < inputs.size(); ++slot) {
            if (!carries[slot] || fed == feeds_.end()) continue;
            const auto link = fed->second.find(inputs[slot]);
            if (link == fed->second.end() || !nodes_.count(link->second)) continue;
            const Channels& from = out(link->second);
            byInput[inputs[slot]] = from;
            for (const auto& [channel, source] : from) incoming.emplace(channel, source);
        }

        Channels made = incoming;
        auto write = [&](const ChannelWrite& w, const Source& source) {
            if (w.name.empty() || w.name[0] == '_') return;
            const auto had = incoming.find(w.name);
            if (had != incoming.end() && !w.extends && !w.param.empty()) {
                info.overwrites.push_back({w.name, w.param, had->second.node});
            }
            info.writes.push_back(w);
            made[w.name] = source;
        };
        const Source self{name, depth_};
        if (type == op::kInput) {
            // A compound's input inside its graph: what reaches the compound there.
            const std::string port = params.is_object() ? params.value("name", std::string()) : std::string();
            if (const auto it = inputs_.find(port); it != inputs_.end()) made = it->second;
        } else if (bypassed) {
            // Its first input as it came.
        } else if (type == op::kGuides) {
            for (const std::string& map : guideMaps_) write({map, ""}, self);
        } else if (type == op::kCompound && depth_ < kMaxDepth) {
            Analysis inner(definition.value("graph", json::object()), guideMaps_, byInput, depth_ + 1, nullptr);
            inner.run();
            for (const auto& [channel, source] : inner.outputChannels()) {
                if (source.depth > depth_) {
                    write({channel, ""}, self);
                } else {
                    made[channel] = source;  // handed through from outside
                }
            }
            for (const ChannelOverwrite& o : inner.innerOverwrites()) info.overwrites.push_back(o);
        } else if (op) {
            for (const ChannelWrite& w : op->channelWrites()) write(w, self);
        }
        if (depth_ > 0) {
            for (const ChannelOverwrite& o : info.overwrites) innerOverwrites_.push_back({o.name, name + "." + o.param, o.node});
        }

        // What its params read, and whether it reaches it.
        if (op && type != op::kCompound && !bypassed) {
            const std::vector<ParamSpec> specs = allParamSpecs(*op);
            for (const ParamSpec& spec : specs) {
                std::string channel;
                if (spec.channel == ChannelUse::Reads) {
                    if (!shown(*op, specs, spec)) continue;
                    channel = op->params().getString(spec.name);
                } else if (op->params().has(spec.name)) {
                    const MapRef* map = std::get_if<MapRef>(&op->params().get(spec.name));
                    if (map && map->source == MapRef::Source::Attribute) channel = map->name;
                }
                if (channel.empty()) continue;
                ChannelRead read{spec.name, channel, false, {}, false};
                if (spec.channel == ChannelUse::Reads && spec.channelOptional) {
                    const std::string* fallback = std::get_if<std::string>(&spec.defaultValue);
                    read.optional = fallback && *fallback == channel;
                }
                if (const auto it = incoming.find(channel); it != incoming.end()) {
                    read.reaches = true;
                    read.node = it->second.node;
                }
                info.reads.push_back(std::move(read));
            }
        }

        if (results_) {
            for (const auto& [channel, source] : incoming) info.incoming.push_back({channel, source.node});
            (*results_)[name] = std::move(info);
        }
        return made;
    }

    const std::vector<std::string>& guideMaps_;
    std::map<std::string, Channels> inputs_;
    int depth_ = 0;
    std::map<std::string, NodeChannels>* results_;
    std::map<std::string, json> nodes_;
    std::map<std::string, std::map<std::string, std::string>> feeds_;  // node -> input -> source
    std::string output_;
    std::map<std::string, Channels> out_;
    std::set<std::string> busy_;
    std::vector<ChannelOverwrite> innerOverwrites_;
};

}  // namespace

std::map<std::string, NodeChannels> graphChannels(const std::string& graphJson, const std::vector<std::string>& guideMaps) {
    const bool blank = graphJson.find_first_not_of(" \t\r\n") == std::string::npos;
    const json graph = json::parse(blank ? std::string("{}") : graphJson, nullptr, false);
    if (graph.is_discarded() || !graph.is_object()) throw GraphError("the graph is not a JSON object");
    std::map<std::string, NodeChannels> results;
    Analysis analysis(graph, guideMaps, {}, 0, &results);
    analysis.run();
    return results;
}

std::string graphChannelsJson(const std::string& graphJson, const std::vector<std::string>& guideMaps) {
    json out = json::object();
    for (const auto& [name, info] : graphChannels(graphJson, guideMaps)) {
        json incoming = json::array(), writes = json::array(), overwrites = json::array(), reads = json::array();
        for (const ChannelSource& c : info.incoming) incoming.push_back({{"name", c.name}, {"node", c.node}});
        for (const ChannelWrite& w : info.writes) writes.push_back({{"name", w.name}, {"param", w.param}, {"extends", w.extends}});
        for (const ChannelOverwrite& o : info.overwrites) {
            overwrites.push_back({{"name", o.name}, {"param", o.param}, {"node", o.node}});
        }
        for (const ChannelRead& r : info.reads) {
            reads.push_back({{"param", r.param}, {"name", r.name}, {"reaches", r.reaches}, {"node", r.node}, {"optional", r.optional}});
        }
        out[name] = {{"incoming", incoming}, {"writes", writes}, {"overwrites", overwrites}, {"reads", reads}};
    }
    return out.dump();
}

}  // namespace groomlab
