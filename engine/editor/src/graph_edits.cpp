// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/graph_edits.h"

#include <exception>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/braid_tubes.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/sculpt.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

std::string followed(std::string name, const std::vector<std::pair<std::string, std::string>>& moved) {
    for (const auto& [from, to] : moved) {
        if (name == from) name = to;
    }
    return name;
}

}  // namespace

GraphFollowers followGraphEdit(const std::string& graphAfter, GraphFollowers followers,
                               const std::vector<std::pair<std::string, std::string>>& moved,
                               const std::map<std::string, std::string>& copied) {
    const json graph = json::parse(graphAfter, nullptr, false);
    const bool readable = graph.is_object() && graph.contains("nodes") && graph["nodes"].is_array();
    std::set<std::string> names;
    if (readable) {
        for (const json& n : graph["nodes"]) {
            if (n.is_object()) names.insert(n.value("name", std::string()));
        }
    }
    // The Sculpt nodes' edits: moved, copied, and the gone nodes' dropped.
    if (!followers.sculpts.empty()) {
        try {
            std::string sculpts = moveSculptNodes(followers.sculpts, moved);
            for (const auto& [copy, source] : copied) sculpts = copySculptNode(sculpts, source, copy);
            if (readable) {
                for (const std::string& node : sculptedNodes(sculpts)) {
                    if (!names.count(node.substr(0, node.find('/')))) sculpts = clearSculptNode(sculpts, node);
                }
            }
            followers.sculpts = std::move(sculpts);
        } catch (const std::exception&) {
            // unreadable: left as it is (the session says why)
        }
    }
    // The brushes' Sculpt node and the viewed node: renamed with theirs, cleared when gone (or
    // grouped: only top-level nodes are shaped and viewed).
    followers.sculptTarget = followed(followers.sculptTarget, moved);
    followers.viewNode = followed(followers.viewNode, moved);
    if (readable) {
        if (!followers.sculptTarget.empty() && !names.count(followers.sculptTarget)) followers.sculptTarget.clear();
        if (!followers.viewNode.empty() && !names.count(followers.viewNode)) followers.viewNode.clear();
    }
    // Show Tubes: a renamed node's name, a removed node's gone (the core's rules).
    if (!followers.braidTubes.empty()) {
        std::string shown = followers.braidTubes;
        for (const auto& [from, to] : moved) shown = renamedBraidTubes(shown, from, to);
        try {
            shown = GraphEdit(graphAfter).braidTubesKept(shown);
        } catch (const std::exception&) {
        }
        followers.braidTubes = std::move(shown);
    }
    return followers;
}

bool wireAfterNode(GraphEdit& edit, const std::string& anchor, const std::string& node) {
    if (!edit.hasNode(anchor) || !edit.hasNode(node) || anchor == node) return false;
    // Into anchor's first outgoing link the node fits in: the chain goes through it.
    const json now = json::parse(edit.toJson());
    for (const json& c : now.value("connections", json::array())) {
        if (!c.is_object() || c.value("from", std::string()) != anchor) continue;
        try {
            GraphEdit trial = edit;
            insertOnLink(trial, anchor, c.value("to", std::string()), c.value("input", std::string()), node, c.value("output", std::string()));
            edit = std::move(trial);
            return true;
        } catch (const std::exception&) {
        }
    }
    // Else fed by anchor on its first input that takes anchor's output.
    const json spec = json::parse(edit.nodeSpecJson(node), nullptr, false);
    for (const json& p : spec.is_object() ? spec.value("inputs", json::array()) : json::array()) {
        try {
            GraphEdit trial = edit;
            trial.connect(anchor, node, p.value("name", std::string()));
            trial.adoptUpstreamClump(node);  // (a Clump after a Clump: inside its clumps, issue #115)
            edit = std::move(trial);
            return true;
        } catch (const std::exception&) {
        }
    }
    return false;
}

}  // namespace groomlab
