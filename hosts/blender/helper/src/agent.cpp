// SPDX-License-Identifier: Apache-2.0
// The agent link in Blender (agent.h): the agent tools over the helper's sessions, each groom's
// edits steps of its session's undo stack.

#include "agent.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "groomlab/editor/agent_link.h"
#include "groomlab/editor/agent_tools.h"
#include "groomlab/frame_time.h"
#include "groomlab/version.h"

#include "pipeline.h"
#include "request.h"
#include "session.h"
#include "tools.h"

namespace groomlab::helper {

using nlohmann::json;
using agent::ToolError;
namespace fs = std::filesystem;

namespace {

// A groom of the scene (its session) as the agent's: read through the session, its edits steps of
// the session's undo stack (tools.h commitStroke / commitStep), its history read from that stack.
class HelperAgentGroom : public agent::AgentGroom {
public:
    HelperAgentGroom(std::string name, Session& s) : AgentGroom(std::move(name)), s_(s), bound_(s.groom.get()) {}
    Session& sessionOf() { return s_; }
    bool boundTo(const Session& s) const { return &s == &s_ && s.groom.get() == bound_; }

    GroomSession& session() override { return *s_.groom; }
    std::string upAxis() const override { return "Y"; }  // groom space (the host's axis turned at the boundary)
    std::string units() const override { return "cm"; }
    double framesPerSecond() const override { return s_.groom->framesPerSecond(); }
    const ViewResult& view(const ViewRequest& request) override {
        // The time's inputs (a guide cache's poses) at the request's time, as evaluate feeds them.
        const double seconds = s_.seconds;
        s_.seconds = request.time;
        feedSessionTime(s_);
        s_.seconds = seconds;
        return s_.groom->evaluate(request);
    }
    GroomFile toFile() override { return sessionGroomFile(s_); }

    void commit(GroomEdit edit) override {
        // What the stored texts do not hold: the reference photos (the file's), put back by undo.
        std::function<void(bool)> side;
        if (edit.referencePhotos) {
            const std::pair<std::string, std::string> photos = *edit.referencePhotos;
            Session* s = &s_;
            side = [s, photos](bool after) { s->file.referencePhotos = after ? photos.second : photos.first; };
        }
        try {
            if (edit.stroke) {
                StrokeEdit stroke = *edit.stroke;
                stroke.mode = edit.title();
                commitStroke(s_, std::move(stroke), side);
            } else {
                std::vector<std::pair<StrokeField, std::string>> texts;
                if (edit.whole) texts.emplace_back(StrokeField::Graph, edit.graphAfter.dump());
                commitStep(s_, edit.title(), texts, side);
            }
        } catch (const std::exception& e) {
            throw ToolError(std::string("the edit was refused, so nothing changed: ") + e.what());
        }
        if (edit.referencePhotos) {
            s_.file.referencePhotos = edit.referencePhotos->second;
            referencePhotos = edit.referencePhotos->second;
        }
    }
    // The session's stack as an EditHistory: each step's label, the last one's texts (what
    // render_preview's compare takes back for its picture).
    EditHistory& history() override {
        history_ = EditHistory{};
        const std::vector<StrokeEdit>& done = doneSteps(s_);
        for (std::size_t i = 0; i < done.size(); ++i) {
            GroomEdit e;
            e.side = true;  // (named by its label; what it holds besides is the session's)
            e.label = done[i].mode;
            if (i + 1 == done.size()) e.stroke = std::make_shared<const StrokeEdit>(done[i]);
            history_.record(std::move(e));
        }
        const std::vector<std::string>& redo = redoLabels(s_);
        for (auto it = redo.rbegin(); it != redo.rend(); ++it) {
            GroomEdit e;
            e.side = true;
            e.label = *it;
            history_.record(std::move(e));
        }
        for (std::size_t i = 0; i < redo.size(); ++i) history_.stepBack();
        return history_;
    }
    std::string undo() override { return undoSession(s_) ? std::string() : std::string("nothing to undo"); }
    std::string redo() override { return redoSession(s_) ? std::string() : std::string("nothing to redo"); }
    void setCollider(const std::string&, MeshPtr) override {
        throw ToolError("in Blender, colliders are Blender meshes: add one with GroomLab > Add > Collider, then name it in a ColliderMesh node");
    }

private:
    Session& s_;
    const GroomSession* bound_;
    EditHistory history_;
};

class HelperAgentTarget : public agent::AgentTarget {
public:
    agent::AgentFolders folders_;
    const agent::AgentFolders& folders() const override { return folders_; }

    // The scene's grooms as the add-on names them (scalp objects -> sessions), and the active one.
    void setGrooms(const std::vector<std::pair<std::string, Session*>>& grooms, const std::string& active) {
        std::vector<std::unique_ptr<HelperAgentGroom>> kept;
        for (const auto& [name, s] : grooms) {
            std::unique_ptr<HelperAgentGroom> g;
            for (auto& old : grooms_) {
                if (old && old->name() == name && old->boundTo(*s)) g = std::move(old);
            }
            if (!g) g = std::make_unique<HelperAgentGroom>(name, *s);
            // (as the session has them now: the add-on's Photos panel and the frame change them)
            g->referencePhotos = s->file.referencePhotos;
            g->frame = secondsToFrame(s->seconds, s->groom->framesPerSecond());
            s->scope.clear();  // (the whole graph: not the compound the Graph Editor is in)
            kept.push_back(std::move(g));
        }
        grooms_ = std::move(kept);
        active_ = active;
    }
    void clear() { grooms_.clear(); }

    std::vector<agent::AgentGroom*> grooms() override {
        std::vector<agent::AgentGroom*> out;
        for (auto& g : grooms_) out.push_back(g.get());
        return out;
    }
    agent::AgentGroom& groom(const std::string& name) override {
        if (grooms_.empty()) throw ToolError("the Blender scene has no groom: make one with GroomLab > Create Groom, or add a sample (GroomLab > Samples)");
        const std::string want = name.empty() ? active_ : name;
        for (auto& g : grooms_) {
            if (g->name() == want) return *g;
        }
        if (name.empty()) {
            if (grooms_.size() == 1) return *grooms_.front();
            throw ToolError("no groom is active in Blender: name one (\"groom\"): list_grooms lists them");
        }
        throw ToolError("the Blender scene has no groom '" + name + "' (list_grooms lists them)");
    }
    std::string opensNoGrooms() const override {
        return "in Blender the agent works on the scene's grooms: make one with GroomLab > Create Groom, or open a .groom file with "
               "GroomLab > Import";
    }
    agent::AgentGroom& open(const std::string&, GroomFile) override { throw ToolError(opensNoGrooms()); }
    void close(const std::string&) override {}
    std::shared_ptr<const ScalpMesh> readMesh(const std::string&, const std::string&) override {
        throw ToolError("in Blender, meshes are Blender objects: import the mesh into the scene, then Create Groom on it");
    }
    agent::ExportReport exportCurves(agent::AgentGroom&, const agent::ExportRequest&) override {
        throw ToolError("in Blender, export the curves with GroomLab > Export (Alembic, USD, V-Ray); save_groom writes a .groom file");
    }

private:
    std::vector<std::unique_ptr<HelperAgentGroom>> grooms_;
    std::string active_;
};

// What a session's stack and texts are, to tell what an answer changed.
struct Before {
    std::size_t done = 0, redo = 0;
    const void* last = nullptr;
    std::string graph, guides, photos;
    MeshPtr scalp;
};

Before before(Session& s) {
    Before b;
    const std::vector<StrokeEdit>& done = doneSteps(s);
    b.done = done.size();
    b.redo = redoLabels(s).size();
    b.last = done.empty() ? nullptr : static_cast<const void*>(&done.back());
    b.graph = s.groom->data().graphJson;
    b.guides = s.groom->data().hierarchyJson;
    b.photos = s.file.referencePhotos;
    b.scalp = s.groom->rawScalp();
    return b;
}

}  // namespace

struct AgentLink {
    HelperAgentTarget target;
    std::unique_ptr<agent::AgentTools> tools;
    std::string token, file;
};

bool isAgentCommand(const std::string& cmd) { return cmd == "agent_start" || cmd == "agent_stop" || cmd == "agent_request"; }

void AgentLinkDeleter::operator()(AgentLink* link) const {
    if (link && !link->file.empty()) agent::removeLinkInfo(link->file);
    delete link;
}

json runAgentCommand(const std::string& cmd, const json& args, std::map<int, std::unique_ptr<Session>>& sessions,
                     AgentLinkPtr& link) {
    if (cmd == "agent_start") {
        const int port = static_cast<int>(optNumber(args, "port", 0.0));
        if (port <= 0 || port > 65535) fail("bad_request", "agent_start needs the \"port\" the add-on listens on");
        const long long pid = static_cast<long long>(optNumber(args, "pid", 0.0));
        if (pid <= 0) fail("bad_request", "agent_start needs Blender's \"pid\"");
        if (link && !link->file.empty()) agent::removeLinkInfo(link->file);
        if (!link) link.reset(new AgentLink());
        AgentLink& l = *link;
        const json folders = args.contains("folders") && args["folders"].is_object() ? args["folders"] : json::object();
        // (canonical: paths an agent gives are compared with it once resolved)
        std::error_code ec;
        const fs::path root = fs::u8path(optString(folders, "root", ""));
        if (root.empty()) fail("bad_request", "agent_start needs \"folders\" {\"root\"}: the folder the agent reads and writes in");
        fs::create_directories(root, ec);
        const fs::path canonical = fs::weakly_canonical(fs::absolute(root, ec), ec);
        l.target.folders_.root = (ec ? root : canonical).u8string();
        l.target.folders_.presetDir = optString(folders, "presets", shippedPresetsFolder());
        l.target.folders_.meshDir = optString(folders, "meshes", sampleMeshesDir());
        l.target.folders_.docsDir = optString(folders, "docs", "");
        if (!l.tools) l.tools = std::make_unique<agent::AgentTools>(l.target);
        agent::LinkInfo info;
        info.host = "blender";
        info.pid = pid;
        info.port = port;
        // (a helper started again keeps the add-on's token: the agent's client keeps working)
        info.token = optString(args, "token", "");
        if (info.token.empty()) info.token = agent::newLinkToken();
        info.groom = optString(args, "groom", "");
        info.started = agent::linkClock();
        info.version = kVersionString;
        try {
            l.file = agent::writeLinkInfo(info);
        } catch (const std::exception& e) {
            fail("io", e.what());
        }
        l.token = info.token;
        return {{"token", l.token}, {"file", l.file}, {"root", l.target.folders_.root}};
    }
    if (cmd == "agent_stop") {
        const bool was = link != nullptr;
        link.reset();
        return {{"stopped", was}};
    }
    // agent_request
    if (!link || link->token.empty()) fail("bad_request", "the agent link is off (agent_start)");
    AgentLink& l = *link;
    const std::string line = needString(args, "line");
    std::vector<std::pair<std::string, Session*>> grooms;
    if (args.contains("grooms") && args["grooms"].is_object()) {
        for (const auto& [name, id] : args["grooms"].items()) {
            if (!id.is_number_integer()) fail("bad_request", "\"grooms\" is {name: session}");
            const auto it = sessions.find(id.get<int>());
            if (it == sessions.end()) fail("no_session", "no session " + id.dump());
            grooms.emplace_back(name, it->second.get());
        }
    }
    l.target.setGrooms(grooms, optString(args, "groom", ""));
    std::vector<Before> was;
    for (const auto& [name, s] : grooms) was.push_back(before(*s));
    const std::string reply = agent::answerLinkRequest(*l.tools, l.token, line);
    // The tool called (the add-on names Blender's undo step after it).
    json tool = nullptr;
    const json request = json::parse(line, nullptr, false);
    if (request.is_object() && request.value("method", std::string()) == "tools/call" && request.contains("params") &&
        request["params"].is_object() && request["params"].contains("name") && request["params"]["name"].is_string()) {
        tool = request["params"]["name"];
    }
    json changed = json::array();
    for (std::size_t i = 0; i < grooms.size(); ++i) {
        Session& s = *grooms[i].second;
        const Before now = before(s);
        const Before& b = was[i];
        const bool stack = now.done != b.done || now.redo != b.redo || now.last != b.last;
        const bool graph = now.graph != b.graph, guides = now.guides != b.guides, photos = now.photos != b.photos,
                   scalp = now.scalp != b.scalp;
        if (!stack && !graph && !guides && !photos && !scalp) continue;
        changed.push_back({{"session", s.id}, {"groom", grooms[i].first}, {"undo", undoStateJson(s)}, {"graphChanged", graph},
                           {"scalpChanged", scalp}, {"photosChanged", photos}});
    }
    return {{"reply", reply}, {"tool", tool}, {"changed", changed}};
}

}  // namespace groomlab::helper
