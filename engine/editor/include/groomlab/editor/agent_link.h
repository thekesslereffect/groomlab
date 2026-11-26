// SPDX-License-Identifier: Apache-2.0
#pragma once

// The agent link (docs: ai-agents.mdx "Live link"): a host running GroomLab (Maya, the app,
// Blender) answers the agent tools for the groom it has open, so an AI agent grooms what the
// artist sees. The host listens on 127.0.0.1 (a port of its own) while its Agent link switch is on
// and says so in a discovery file; `groomlab-cli mcp --live` (the stdio server the agent's client
// starts) finds the file, connects and forwards the MCP tools/list and tools/call to it.
//
// The protocol, every host's (this header: no sockets, no Qt; the host reads and writes the lines
// with its own sockets):
//   - JSON lines both ways (UTF-8, one object a line, "\n").
//   - A request: {"token": "<the discovery file's>", "id": 1, "method": "tools/list" | "tools/call"
//     | "ping", "params": {...}} (tools/call's params: {"name", "arguments"}, as MCP's).
//   - The answer: {"id": 1, "result": {...}} (tools/list: {"tools": [...]}; tools/call: {"content":
//     [...], "isError"}: AgentTools' own result, images included), or {"id": 1, "error":
//     {"code", "message"}}. A request without the right token is refused (code -32001) and answers
//     nothing else.
//   - Discovery: <agentLinkDir()>/<host>-<pid>.json, {"host", "pid", "port", "token", "groom",
//     "started" (seconds since 1970), "version"}, written when the link starts and removed when it
//     stops; a file whose process is gone is stale (ignored, and removed by whoever reads it).

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace groomlab::agent {

class AgentTools;

constexpr int kLinkBadToken = -32001;
constexpr int kLinkBadRequest = -32600;
constexpr int kLinkUnknownMethod = -32601;
constexpr std::size_t kMaxLinkLine = 64u << 20;  // the longest line either side reads

// A running host's link, as its discovery file says.
struct LinkInfo {
    std::string host;   // maya, blender, app
    long long pid = 0;
    int port = 0;
    std::string token;
    std::string groom;  // the groom it has open now ("" none)
    double started = 0.0;  // seconds since 1970
    std::string version;
    std::string file;   // the discovery file read (not written into it)

    nlohmann::json toJson() const;
    static LinkInfo fromJson(const nlohmann::json& j);  // throws std::invalid_argument
};

// Where discovery files go: $GROOMLAB_AGENT_LINK_DIR when set, else <home>/Documents/GroomLab/
// agent_link (beside the user's presets and nodes). "" when no home is known.
std::string agentLinkDir();
// A fresh random token (32 hex digits).
std::string newLinkToken();
// Seconds since 1970, now.
double linkClock();
// Whether process `pid` is running.
bool processAlive(long long pid);

// Writes `info`'s discovery file (<dir>/<host>-<pid>.json; the folder made), returns its path.
// Throws std::runtime_error when it cannot be written.
std::string writeLinkInfo(const LinkInfo& info, const std::string& dir = agentLinkDir());
// Removes a discovery file (nothing when it is not there).
void removeLinkInfo(const std::string& path);
// The live hosts' links in `dir`, newest first: each file read, a stale one (its process gone, or
// not a link file) removed and left out. `host` "" any, else only that host's.
std::vector<LinkInfo> liveLinks(const std::string& host = {}, const std::string& dir = agentLinkDir());

// The host's side: one request line answered by `tools` (the answer line, no "\n"): the token
// checked, tools/list and tools/call run, ping answered. Never throws.
std::string answerLinkRequest(AgentTools& tools, const std::string& token, const std::string& line);
// The client's side: a request line, and the answer's result (the "result" object) from its line;
// throws std::runtime_error with the host's message for an error answer or a line that is no answer.
std::string linkRequestLine(const std::string& token, std::int64_t id, const std::string& method, const nlohmann::json& params);
nlohmann::json linkResult(const std::string& line, std::int64_t id);

}  // namespace groomlab::agent
