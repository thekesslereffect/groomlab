// SPDX-License-Identifier: Apache-2.0
#pragma once

// `groomlab-cli mcp`: a Model Context Protocol server on stdin and stdout, so AI apps (Claude
// Desktop and Code, Cursor, VS Code's agents...) launch it and drive GroomLab through its tools
// (engine/editor agent_tools.h) on grooms it holds in memory (mcp_groom.h), or with --live on the
// groom open in a running host (Maya, the app, Blender: engine/editor agent_link.h), forwarding the
// tool calls to it over 127.0.0.1. JSON-RPC 2.0, one message per line; stdout carries only the
// protocol, logging goes to stderr.

#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/editor/agent_tools.h"

namespace groomlab::cli {

// The newest protocol revision the server speaks, and the ones it accepts from a client.
constexpr const char* kMcpProtocolVersion = "2025-06-18";

// What answers the tools: the server's own AgentTools (headless), or a host's over the link.
class ToolBackend {
public:
    virtual ~ToolBackend() = default;
    virtual nlohmann::json list() = 0;
    virtual bool has(const std::string& name) = 0;
    virtual agent::ToolResult call(const std::string& name, const nlohmann::json& arguments) = 0;
    // What the server's instructions say of where the tools run ("" nothing).
    virtual std::string about() { return {}; }
};

// The server's own tools.
class HeadlessBackend : public ToolBackend {
public:
    explicit HeadlessBackend(agent::AgentTools& tools) : tools_(tools) {}
    nlohmann::json list() override { return tools_.list(); }
    bool has(const std::string& name) override { return tools_.has(name); }
    agent::ToolResult call(const std::string& name, const nlohmann::json& arguments) override { return tools_.call(name, arguments); }

private:
    agent::AgentTools& tools_;
};

// One incoming message (a request, a notification or a batch of them) handled: the response to
// send, or null for none (notifications). Never throws.
nlohmann::json handleMcpMessage(ToolBackend& tools, const nlohmann::json& message);

// Runs the server until stdin ends: `groomlab-cli mcp [--root <dir>] [--live [maya|blender|app]]`.
// Returns the exit code.
int runMcpServer(int argc, char** argv);

}  // namespace groomlab::cli
