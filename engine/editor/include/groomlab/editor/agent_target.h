// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the agent tools (agent_tools.h) work on: an AgentTarget holds the grooms an AI agent edits
// and says where it may read and write files. groomlab-cli's MCP server is one (headless: grooms it
// opens in memory, engine/cli mcp_groom.h); a host's live link will be another (the groom it has
// open, edited through its own undo). Each groom is an AgentGroom: its data and evaluation (a
// GroomSession), and its edits, each one undo step (GroomEdit, EditHistory: groom_edit.h).

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/editor/agent_preview.h"
#include "groomlab/editor/groom_edit.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/stroke.h"

namespace groomlab::agent {

// What a tool's arguments or the groom got wrong, said for the agent.
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The most undo steps a groom keeps.
constexpr std::size_t kMaxUndo = 64;

// The folder an agent's files are read and written in, and GroomLab's own resources.
struct AgentFolders {
    std::string root;       // files are read and written only under this folder (absolute)
    std::string presetDir;  // the shipped presets, sample meshes and docs pages ("" none)
    std::string meshDir;
    std::string docsDir;

    // A path an agent gave, made absolute under the root (a relative one joins it): ToolError when
    // it lies outside the root once ".." and links are resolved, or is empty.
    std::string resolve(const std::string& path) const;
    // The same for a file to write: its folder is made (inside the root).
    std::string resolveForWrite(const std::string& path) const;
    // Whether a path lies under the root (absolute, or relative to it).
    bool insideRoot(const std::string& path) const;
    // `path` shown relative to the root when it is inside it.
    std::string shown(const std::string& path) const;
    // The user's presets, then the shipped ones.
    std::vector<std::string> presetDirs() const;
};

class AgentGroom;

// The StrokeHost a groom's scripted strokes run on (scriptedStroke, the brush_stroke and paint_map
// tools): the session's, with what a step sets besides the stroke staged first (a map made, a
// param linked to it: `stage`), read by the stroke as stored. commit makes the stroke one undo step
// of the groom (AgentGroom::commit) named `label`, the staged texts in it too; texts the stroke
// left as they were are dropped, and a stroke that changed nothing is no step.
class AgentStrokeHost : public SessionStrokeHost {
public:
    explicit AgentStrokeHost(AgentGroom& groom);

    std::string label = "stroke";
    // A text set for the coming stroke (and its step) without changing the groom yet.
    void stage(StrokeField field, std::string text);
    void clearStage();
    // Whether the last commit made a step.
    bool committed() const { return committed_; }
    void resetCommitted() { committed_ = false; }

    std::shared_ptr<const GuideHierarchy> authoredHierarchy() override;
    std::string storedText(StrokeField field) override;
    const ScalpMaps& scalpMaps() override;
    void commit(StrokeEdit edit) override;

private:
    AgentGroom& groom_;
    StrokeEdit staged_;
    std::shared_ptr<const GuideHierarchy> stagedHierarchy_;
    ScalpMaps stagedMaps_;
    bool committed_ = false;
};

// One groom an agent works on. Its edits are GroomEdits (an agent's are StrokeEdits of the stored
// texts: graph, guides, maps, parts, flow), applied by `apply` (EditorHost) and kept in its history.
class AgentGroom : public EditorHost {
public:
    explicit AgentGroom(std::string name);
    ~AgentGroom() override;
    AgentGroom(const AgentGroom&) = delete;
    AgentGroom& operator=(const AgentGroom&) = delete;

    const std::string& name() const { return name_; }

    // The groom's data (graph, guides, scalp, maps) and its evaluation.
    virtual GroomSession& session() = 0;
    virtual std::string upAxis() const = 0;  // "Y" or "Z"
    virtual std::string units() const = 0;
    virtual double framesPerSecond() const = 0;
    // The groom evaluated for a view at request.time, what that time reads fed first (caches).
    virtual const ViewResult& view(const ViewRequest& request) = 0;
    // What save_groom writes: the groom's file as it is now.
    virtual GroomFile toFile() = 0;

    // EditorHost: the edit's texts put back on the session, the graph first ("" or why it would
    // not load: then nothing changed).
    std::string apply(const GroomEdit& edit, bool after) override;
    // A reference photos edit (the groom's referencePhotos text from `before` to `after`): one
    // undo step named `label`.
    void commitPhotos(const std::string& label, const std::string& after);
    // An edit applied and recorded as one undo step (the oldest past kMaxUndo dropped). Throws
    // ToolError (nothing changed) when the host cannot apply it.
    void commit(GroomEdit edit);
    EditHistory& history() { return history_; }
    // Undo / redo one step: "" or the host's error.
    std::string undo();
    std::string redo();
    // The host strokes run on (default: an AgentStrokeHost on session()).
    virtual AgentStrokeHost& strokeHost();

    std::string path;                // the .groom file it was opened from or last saved to ("" none)
    std::string referencePhotos;     // the reference photos (groom_file.h "referencePhotos": agent_photos.h)
    // How previews show the tube hierarchy (display.h): the Lock view's group and the Mixed view.
    std::string tubeViewGroup, mixedView;
    std::string mesh;                // what it was made on (a new groom)
    double frame = 1.0;              // the frame it is evaluated at
    std::vector<GuideId> selection;  // the guides select_guides picked

protected:
    // The session was replaced (caches opened again): the stroke host made on the old one goes.
    void dropStrokeHost() { strokes_.reset(); }

private:
    std::string name_;
    EditHistory history_;
    std::unique_ptr<AgentStrokeHost> strokes_;
};

// What a groom's animated inputs come from (groom_file.h caches): set where given.
struct AnimationInputs {
    std::optional<CacheRef> scalpCache;  // the scalp over time (the same vertices); empty file: none
    std::optional<CacheRef> guideCache;  // guide poses over time, by guide ID; empty file: none
};

// What export writes.
struct ExportRequest {
    std::string path;  // .abc, .usda, .usd or .vrscene (absolute, inside the root)
    double start = 1.0, end = 1.0, step = 1.0;
    float density = 1.0f;
    std::string scales = "mesh";
};

struct ExportReport {
    std::size_t frames = 0;
    std::size_t strandTotal = 0;  // over every frame
    double milliseconds = 0.0;
    std::string format;  // alembic, usd or vrscene
};

// The grooms an agent works on, and what needs the host's own readers and writers.
class AgentTarget {
public:
    virtual ~AgentTarget() = default;

    virtual const AgentFolders& folders() const = 0;

    // The grooms open, by name.
    virtual std::vector<AgentGroom*> grooms() = 0;
    // The groom named (ToolError for none); with `name` empty, the one groom open.
    AgentGroom& groom(const std::string& name);
    AgentGroom* find(const std::string& name);
    // A groom from file data opened under `name` (new_groom, open_groom), replacing one of that
    // name. Throws ToolError where the target opens no grooms of its own, or what reading the
    // file's caches throws.
    virtual AgentGroom& open(const std::string& name, GroomFile file) = 0;
    virtual void close(const std::string& name) = 0;

    // A scalp from a mesh file (.obj, .abc; `object`: the Alembic mesh's name, "" the first).
    // Throws ToolError.
    virtual std::shared_ptr<const ScalpMesh> readMesh(const std::string& path, const std::string& object) = 0;
    // The groom's strands written as curves over frames (export). Throws ToolError.
    virtual ExportReport exportCurves(AgentGroom& groom, const ExportRequest& request) = 0;
    // ---- Animation and simulation (Alembic: the CLI's; others refuse, ToolError) ----
    // The groom's animated inputs replaced (its caches opened again; its edits and history kept).
    virtual void setAnimation(AgentGroom& groom, const AnimationInputs& inputs);
    // An animated mesh written as a scalp cache: `frames` (the same topology) `secondsPerFrame`
    // apart from `startSeconds`.
    virtual void writeMeshCache(const std::string& path, const std::string& name,
                                const std::vector<std::shared_ptr<const ScalpMesh>>& frames, double startSeconds,
                                double secondsPerFrame);
    // Simulate node `node`'s frames first..last written to `path` (simulated first) and read back as
    // its bake (saved with the groom). Returns the frames written.
    virtual std::size_t bakeSimulation(AgentGroom& groom, const std::string& node, const std::string& path, int first, int last);
    // A simulation cache read as node `node`'s bake (made with its settings as they are now).
    virtual void loadSimulationCache(AgentGroom& groom, const std::string& node, const std::string& path, const std::string& object);

    // A picture of the groom (render_preview): the CPU preview by default. Throws
    // std::invalid_argument for options it cannot draw.
    virtual PreviewImage preview(AgentGroom& groom, const ScalpMesh* scalp, const StrandSet* strands, const PreviewOptions& options);
};

}  // namespace groomlab::agent
