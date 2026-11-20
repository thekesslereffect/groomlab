// SPDX-License-Identifier: Apache-2.0
#pragma once

// Attributes: the named values per strand or per point the strands carry ("channels" in the code:
// StrandSet's strand and point channels). Some nodes write them (a Clump its clumpChannel, clumpId
// by default; an Attribute node its name; a Flyaway always "flyaway"), others read them by name
// (a Curl's clumpChannel, a Clump's regionChannel, an attribute map on any mappable param). A node
// writing a name an upstream node wrote overwrites it: chained Clumps each want their own name.
//
// graphChannels reads a graph's JSON (graph_io.h's schema, as every host's node editor holds it),
// without evaluating it, and tells for each node which channels reach it (and from which node),
// which it writes, which upstream channel it overwrites, and whether each channel its params read
// reaches it. A graph that does not load yet (an input unconnected, no output) is read as it is:
// what is not connected brings nothing. What the editors show beside the channel params
// (ParamSpec::channel): "Writes"/"Reads", the channels to pick from, an overwrite warning, a note
// for a read that nothing upstream writes.

#include <map>
#include <string>
#include <vector>

#include "groomlab/graph.h"

namespace groomlab {

// A channel and the node that wrote it last (a compound's name for a node inside it).
struct ChannelSource {
    std::string name;
    std::string node;
};

// A node writing a channel an upstream node wrote (`node`: that one), through its param `param`.
struct ChannelOverwrite {
    std::string name;
    std::string param;
    std::string node;
};

// A channel a node reads: through a param marked readsChannel (shown as its node is set, holding
// a name) or an attribute map on a mappable param. `reaches`: some node upstream writes it
// (`node`: the last one); false, the node finds no such channel and ignores it. `optional`: the
// param holds its default and the node does without that channel by design
// (ParamSpec::channelOptional: Clump's regionChannel region), so one missing is no mistake.
struct ChannelRead {
    std::string param;
    std::string name;
    bool reaches = false;
    std::string node;
    bool optional = false;
    // An optional read its user leaves at the default too (ParamSpec::channelFor): nothing to note
    // until one of them changes (a fresh groom's HairColor with no Clump before it). Every host's
    // Node panel shows no line for it.
    bool quiet = false;
};

struct NodeChannels {
    std::vector<ChannelSource> incoming;  // reaching its inputs (those handing channels on), by name
    std::vector<ChannelWrite> writes;     // what it writes as set (none when bypassed)
    std::vector<ChannelOverwrite> overwrites;
    std::vector<ChannelRead> reads;
};

// Every node of the graph by name. `guideMaps`: the maps painted on the guides (layers), which the
// Guides node carries and an Interpolate (or a ToStrands with channels) turns into channels. A
// bypassed node writes nothing and hands its first input's channels on. A compound's inner nodes
// are read through its definition: it writes what they write, and reports their overwrites with
// the inner node's name ("inner.param"). Names starting with "_" are nodes' own and never listed.
// Throws GraphError when the text is not a JSON object.
std::map<std::string, NodeChannels> graphChannels(const std::string& graphJson, const std::vector<std::string>& guideMaps = {});

// The same as JSON, for hosts:
//   {"clump2": {"incoming": [{"name": "clumpId", "node": "clump1"}, ...],
//               "writes": [{"name": "clumpId", "param": "clumpChannel", "extends": false}, ...],
//               "overwrites": [{"name": "clumpId", "param": "clumpChannel", "node": "clump1"}],
//               "reads": [{"param": "regionChannel", "name": "region", "reaches": false, "node": "",
//                          "optional": true}]}, ...}
std::string graphChannelsJson(const std::string& graphJson, const std::vector<std::string>& guideMaps = {});

}  // namespace groomlab
