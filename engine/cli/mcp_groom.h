// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the MCP tools work on (mcp_tools.cpp): the folder files may be read and written in, where
// GroomLab's own resources are, and the grooms open in memory with their undo stacks.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/groom_scene.h"

namespace groomlab::cli {

// What a tool's arguments or the groom got wrong, said for the agent.
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The most undo steps a groom keeps.
constexpr std::size_t kMaxUndo = 64;

// One step back: the graph and the guides before an edit.
struct GroomSnapshot {
    std::string label, graph, hierarchy;
};

struct Groom {
    std::string name;
    std::unique_ptr<GroomScene> scene;
    std::vector<GroomSnapshot> undo;
    std::string path;  // the .groom file it was opened from or last saved to ("" none)
    std::string mesh;  // what it was made on (a new groom)
    double frame = 1.0;
};

class Workspace {
public:
    Workspace(std::string root, std::string exeDir);

    const std::string& root() const { return root_; }
    // A path an agent gave, made absolute under the root (a relative one joins it): ToolError when
    // it lies outside the root once ".." and links are resolved, or is empty.
    std::string resolve(const std::string& path) const;
    // The same for a file to write: its folder is made (inside the root).
    std::string resolveForWrite(const std::string& path) const;
    // Whether a path lies under the root (absolute, or relative to it).
    bool insideRoot(const std::string& path) const;
    // `path` shown relative to the root when it is inside it.
    std::string shown(const std::string& path) const;

    // GroomLab's own folders: beside the executable when installed, else the source tree's.
    const std::string& meshDir() const { return meshDir_; }
    const std::string& docsDir() const { return docsDir_; }
    std::vector<std::string> presetDirs() const;  // the user's presets, then the shipped ones

    // ---- Grooms ----
    std::map<std::string, Groom>& grooms() { return grooms_; }
    // The groom named (ToolError for none); with `name` empty, the one groom open.
    Groom& groom(const std::string& name);
    Groom& add(const std::string& name, std::unique_ptr<GroomScene> scene);

    // The groom's graph as JSON text (the default graph for an empty one) and its guide hierarchy's.
    static std::string graphText(const Groom& g);
    static std::string hierarchyText(const Groom& g);
    // The scalp size presets and new nodes are fitted to (presetScalpSize, the groom's own scale
    // divided out); nullopt without a scalp.
    static std::optional<double> fitSize(const Groom& g);

    // An edit: `change` gets copies of the graph and the hierarchy JSON and changes them; when it
    // returns, the old ones are kept for undo and the new ones go to the groom (ToolError, nothing
    // changed, when the graph would not load).
    void edit(Groom& g, const std::string& label, const std::function<void(std::string& graph, std::string& hierarchy)>& change);
    // Undoes the last `steps` edits; returns their labels (newest first).
    std::vector<std::string> undo(Groom& g, int steps);

    // Evaluates the groom (at its frame) and says how it went: strand and point counts, bounds,
    // errors, the nodes' notes, the time.
    nlohmann::json summary(Groom& g, float density = 1.0f, const std::string& viewNode = {}, bool timings = false);

private:
    std::string root_;
    std::string presetDir_, meshDir_, docsDir_;
    std::map<std::string, Groom> grooms_;
};

}  // namespace groomlab::cli
