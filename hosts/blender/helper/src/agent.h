// SPDX-License-Identifier: Apache-2.0
#pragma once

// The agent link in Blender (docs/BLENDER.md "Agent link"; engine/editor agent_link.h,
// agent_tools.h): the add-on listens on 127.0.0.1 and hands each request line to the helper, which
// answers it with the agent tools on the scene's grooms, its sessions. Each groom's edits are steps
// of its session's undo stack (the tools' own: Ctrl+Z in the Groom Tools, and Blender's undo that
// follows it), so an agent's undo, redo and history are the artist's.
//
//   agent_start {port, pid, folders {root, docs?}, token?, groom?}  -> {token, file}
//   agent_stop                                                      -> {stopped}
//   agent_request {line, grooms {name: session}, groom (the active one's name)}
//       -> {reply (the answer line), tool, changed [{session, undo, graphChanged, scalpChanged,
//           photosChanged}]}
//   agent_render_begin {line, grooms, groom}: a render_frame call checked -> {render {groom,
//       session, renderer, width, height, quality, samples, frame, camera}, id} or {reply}
//   agent_render_end {begun, png, seconds, info, error} -> {reply}: the PNG the add-on rendered

#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace groomlab::helper {

struct Session;
struct AgentLink;  // agent.cpp
struct AgentLinkDeleter {
    void operator()(AgentLink* link) const;  // (its file removed)
};
using AgentLinkPtr = std::unique_ptr<AgentLink, AgentLinkDeleter>;

bool isAgentCommand(const std::string& cmd);
nlohmann::json runAgentCommand(const std::string& cmd, const nlohmann::json& args,
                               std::map<int, std::unique_ptr<Session>>& sessions, AgentLinkPtr& link);

}  // namespace groomlab::helper
