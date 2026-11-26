// SPDX-License-Identifier: Apache-2.0
#pragma once

// The agent tools: what an AI agent can do with GroomLab (groomlab-cli's MCP server speaks the
// protocol, engine/cli mcp_server.h), each tool's schema and description written for a language
// model, worked out once against an AgentTarget (agent_target.h): the grooms it works on, by name
// (a scalp, a guide hierarchy and a graph: what a .groom file holds), each with an undo history.
// Every tool that changes one answers with a short summary of the groom after the change (strands,
// points, errors). Files are read and written only under the target's root; GroomLab's own
// presets, nodes, sample meshes and docs are read where they ship.

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/editor/agent_target.h"

namespace groomlab::agent {

// A tool's answer: MCP content blocks (text, image), and whether it is an error (the agent reads
// the message and tries again; a protocol error is something else).
struct ToolResult {
    nlohmann::json content = nlohmann::json::array();
    bool isError = false;
};

class AgentTools {
public:
    explicit AgentTools(AgentTarget& target);
    ~AgentTools();
    AgentTools(const AgentTools&) = delete;
    AgentTools& operator=(const AgentTools&) = delete;

    // tools/list's "tools": each tool's name, title, description and input JSON Schema.
    nlohmann::json list() const;
    bool has(const std::string& name) const;
    // Runs a tool. Never throws for what the arguments or the groom get wrong (an error result).
    ToolResult call(const std::string& name, const nlohmann::json& arguments);

    AgentTarget& target();

    // render_frame's arguments checked into a request (ToolError: what is wrong, said for the agent)
    // and its answer: for a host whose own glue renders (agent_link.h beginHostRender).
    RenderRequest renderRequest(const nlohmann::json& arguments);
    ToolResult renderResult(const RenderRequest& request, const RenderedFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace groomlab::agent
