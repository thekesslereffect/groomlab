// SPDX-License-Identifier: Apache-2.0
#pragma once

// The MCP server's headless target (agent_target.h): the agent tools (engine/editor agent_tools.h)
// run on grooms held in memory by name, each a groom file ready to evaluate (GroomScene) with its
// undo history. Files are read and written only under the root the server was started with;
// GroomLab's own presets, nodes, sample meshes and docs are found beside the executable (or in the
// source tree for a build tree's). What needs Alembic stays here: reading .abc scalps, opening a
// groom's caches, exporting curves.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/editor/agent_target.h"
#include "groomlab/groom_scene.h"

namespace groomlab::cli {

// A groom open in the server: a GroomScene (its caches read at its frame).
class HeadlessGroom : public agent::AgentGroom {
public:
    HeadlessGroom(std::string name, std::unique_ptr<GroomScene> scene);

    GroomSession& session() override { return scene_->session(); }
    std::string upAxis() const override { return scene_->file().upAxis; }
    std::string units() const override { return scene_->file().units; }
    double framesPerSecond() const override { return scene_->framesPerSecond(); }
    // Throws agent::ToolError ("alembic: ...") for a cache that cannot be read.
    const ViewResult& view(const ViewRequest& request) override;
    GroomFile toFile() override;

    GroomScene& scene() { return *scene_; }
    // The scene made again from the groom as it is, its caches replaced where given (agent::ToolError
    // for a cache that cannot be read: the groom left as it was).
    void reopen(const agent::AnimationInputs& inputs);
    // The Simulate nodes' bakes the groom names (saved with it).
    std::map<std::string, SimCacheRef> bakes;

private:
    std::unique_ptr<GroomScene> scene_;
};

class HeadlessTarget : public agent::AgentTarget {
public:
    // `root`: the folder files are read and written in (absolute); `exeDir`: the executable's
    // folder, GroomLab's shipped presets, nodes, meshes and docs beside it.
    HeadlessTarget(std::string root, const std::string& exeDir);

    const agent::AgentFolders& folders() const override { return folders_; }
    std::vector<agent::AgentGroom*> grooms() override;
    agent::AgentGroom& open(const std::string& name, GroomFile file) override;
    void close(const std::string& name) override;
    std::shared_ptr<const ScalpMesh> readMesh(const std::string& path, const std::string& object) override;
    agent::ExportReport exportCurves(agent::AgentGroom& groom, const agent::ExportRequest& request) override;
    void setAnimation(agent::AgentGroom& groom, const agent::AnimationInputs& inputs) override;
    void writeMeshCache(const std::string& path, const std::string& name, const std::vector<std::shared_ptr<const ScalpMesh>>& frames,
                        double startSeconds, double secondsPerFrame) override;
    std::size_t bakeSimulation(agent::AgentGroom& groom, const std::string& node, const std::string& path, int first, int last) override;
    void loadSimulationCache(agent::AgentGroom& groom, const std::string& node, const std::string& path, const std::string& object) override;

private:
    agent::AgentFolders folders_;
    std::map<std::string, std::unique_ptr<HeadlessGroom>> grooms_;
};

}  // namespace groomlab::cli
