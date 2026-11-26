// SPDX-License-Identifier: Apache-2.0
#pragma once

// groomlab-helper's requests (docs/BLENDER.md, "Protocol"): one JSON object per line in, one per
// line out. Helper::handle takes a request and gives its response; it never throws.

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "agent.h"

namespace groomlab::helper {

// The protocol's version: a client checks it in hello's answer. A new command or field keeps it;
// a change an older client would misread raises it.
inline constexpr int kProtocolVersion = 1;
// The tools' commands (docs/BLENDER.md "Tools"), answered in hello's "features": an add-on that
// draws GroomLab's own tools checks it.
inline constexpr int kToolsVersion = 1;

// An error a request answers with: its code ("bad_request", "not_found", "graph"...) and message.
class RequestError : public std::runtime_error {
public:
    RequestError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

struct Session;

// The folder of the helper's executable: shipped presets are looked up beside it.
void setExecutableDir(const std::string& dir);

class Helper {
public:
    Helper();
    ~Helper();

    // The response to one request ({"id", "cmd", ...}): {"id", "ok": true, "result": {...}} or
    // {"id", "ok": false, "error": {"code", "message"}}.
    nlohmann::json handle(const nlohmann::json& request);
    // The response to a line that is not a JSON object.
    static nlohmann::json malformed(const std::string& why);
    // A shutdown request was answered: the loop ends.
    bool done() const { return done_; }

private:
    nlohmann::json run(const std::string& cmd, const nlohmann::json& args);
    Session& session(const nlohmann::json& args);

    std::map<int, std::unique_ptr<Session>> sessions_;
    AgentLinkPtr agent_;  // the agent link while the add-on has it on (agent.h)
    int nextSession_ = 1;
    bool done_ = false;
};

}  // namespace groomlab::helper
