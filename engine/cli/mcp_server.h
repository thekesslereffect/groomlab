// SPDX-License-Identifier: Apache-2.0
#pragma once

// `groomlab-cli mcp`: a Model Context Protocol server on stdin and stdout, so AI apps (Claude
// Desktop and Code, Cursor, VS Code's agents...) launch it and drive GroomLab through its tools
// (engine/editor agent_tools.h) on grooms it holds in memory (mcp_groom.h). JSON-RPC 2.0, one message per line; stdout carries only the protocol, logging
// goes to stderr. No network.

#include <string>

#include <nlohmann/json.hpp>

namespace groomlab::agent {
class AgentTools;
}

namespace groomlab::cli {

// The newest protocol revision the server speaks, and the ones it accepts from a client.
constexpr const char* kMcpProtocolVersion = "2025-06-18";

// One incoming message (a request, a notification or a batch of them) handled: the response to
// send, or null for none (notifications). Never throws.
nlohmann::json handleMcpMessage(agent::AgentTools& tools, const nlohmann::json& message);

// Runs the server until stdin ends: `groomlab-cli mcp [--root <dir>]`. Returns the exit code.
int runMcpServer(int argc, char** argv);

}  // namespace groomlab::cli
