// SPDX-License-Identifier: Apache-2.0
#pragma once

// The client's side of the agent link (engine/editor agent_link.h): a TCP connection to a host's
// link on 127.0.0.1, one JSON line out and one back. `groomlab-cli mcp --live` forwards the agent's
// tool calls through it (mcp_server.cpp).

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/editor/agent_link.h"

namespace groomlab::cli {

// The connection failed or closed (the host went away), as against an error the host answered.
class LinkLost : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LinkClient {
public:
    // Connects to `info`'s port on 127.0.0.1. Throws LinkLost.
    explicit LinkClient(agent::LinkInfo info);
    ~LinkClient();
    LinkClient(const LinkClient&) = delete;
    LinkClient& operator=(const LinkClient&) = delete;

    const agent::LinkInfo& info() const { return info_; }
    // A request answered: its result. Throws LinkLost (the connection), std::runtime_error (the
    // host's error answer).
    nlohmann::json request(const std::string& method, const nlohmann::json& params = nullptr);

private:
    struct Socket;
    agent::LinkInfo info_;
    std::unique_ptr<Socket> socket_;
    std::int64_t nextId_ = 1;
    std::string buffer_;  // what was read past the last line
};

}  // namespace groomlab::cli
