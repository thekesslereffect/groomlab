// SPDX-License-Identifier: Apache-2.0
#include "agent_cmd.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <maya/M3dView.h>
#include <maya/MAnimControl.h>
#include <maya/MArgDatabase.h>
#include <maya/MDagPath.h>
#include <maya/MDistance.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MImage.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MObjectHandle.h>
#include <maya/MPlug.h>
#include <maya/MTime.h>
#include <maya/MUuid.h>

#include <nlohmann/json.hpp>

#include "active_groom.h"
#include "command_args.h"
#include "groom_shape.h"
#include "groomlab/editor/agent_link.h"
#include "groomlab/editor/agent_tools.h"
#include "groomlab/png.h"
#include "groomlab/version.h"
#include "guarded.h"
#include "maya_inputs.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace groomlab::maya {

namespace {

using nlohmann::json;
using agent::AgentGroom;
using agent::ToolError;

AgentCmd* gCurrent = nullptr;

double mayaFps() { return MTime(1.0, MTime::kSeconds).as(MTime::uiUnit()); }

std::string mayaUnits() {
    switch (MDistance::uiUnit()) {
        case MDistance::kMillimeters: return "mm";
        case MDistance::kMeters: return "m";
        case MDistance::kInches: return "in";
        case MDistance::kFeet: return "ft";
        case MDistance::kYards: return "yd";
        default: return "cm";
    }
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// A groom of the scene: its shape's session read (the live evaluation, what the viewport shows),
// its edits set on its plugs through the running groomLabAgent's modifier (Maya's undo).
class MayaGroom : public AgentGroom {
public:
    MayaGroom(std::string name, const MObject& node) : AgentGroom(std::move(name)), node_(node) {}

    bool alive() const { return node_.isValid() && node_.isAlive(); }
    MObject node() const { return node_.object(); }

    GroomShape* shape() {
        if (!alive()) throw ToolError("the groom '" + name() + "' is gone from the scene");
        auto* s = dynamic_cast<GroomShape*>(MFnDagNode(node_.object()).userNode());
        if (!s) throw ToolError("'" + name() + "' is not a GroomLab groom");
        return s;
    }

    GroomSession& session() override {
        GroomShape* s = shape();
        s->ensureEvaluated();
        return const_cast<GroomSession&>(s->session());
    }
    std::string upAxis() const override { return MGlobal::isZAxisUp() ? "Z" : "Y"; }
    std::string units() const override { return mayaUnits(); }
    double framesPerSecond() const override { return mayaFps(); }

    // The groom as the viewport shows it now (Maya's current time and the viewport's density);
    // a node's strands evaluated on their own when asked.
    const ViewResult& view(const ViewRequest& request) override {
        GroomShape* s = shape();
        s->ensureEvaluated();
        const FramePtr shown = s->frame();
        if (!shown) throw ToolError("the groom has not evaluated yet: is its scalp mesh connected?");
        view_ = shown->view;
        view_.errors = lines(shown->errors);
        if (!request.viewNode.empty()) {
            try {
                const Value value = s->evaluateNode(request.viewNode, request.density);
                const StrandsPtr* strands = std::get_if<StrandsPtr>(&value);
                if (!strands) throw ToolError("'" + request.viewNode + "' gives no strands");
                view_.strands = *strands;
                view_.follow = nullptr;
                view_.viewedNode = request.viewNode;
            } catch (const GraphError& e) {
                throw ToolError(std::string("graph: ") + e.what());
            }
        }
        return view_;
    }

    GroomFile toFile() override {
        GroomFile file;
        file.data = groomDataFromNode(node_.object());
        file.framesPerSecond = mayaFps();
        file.upAxis = upAxis();
        file.units = mayaUnits();
        file.referencePhotos = referencePhotos;
        file.source = "Maya " + name();
        return file;
    }

    // The edit's texts set on the groom's plugs (guideData, graph, scalpMaps...: strokeFieldName)
    // through the running command's modifier, so Maya undoes and redoes them with the chunk.
    std::string apply(const GroomEdit& edit, bool after) override {
        AgentCmd* cmd = AgentCmd::current();
        if (!cmd) return "no groomLabAgent command is running to record the edit";
        if (edit.referencePhotos) referencePhotos = after ? edit.referencePhotos->second : edit.referencePhotos->first;
        MFnDagNode fn(node_.object());
        struct Set {
            MPlug plug;
            std::string text;
            bool graph;
        };
        std::vector<Set> sets;
        if (edit.whole) sets.push_back({fn.findPlug("graph", true), (after ? edit.graphAfter : edit.graphBefore).dump(), true});
        if (edit.stroke) {
            for (std::size_t i = 0; i < kStrokeFields; ++i) {
                const StrokeEdit::Text& t = edit.stroke->texts[i];
                if (!t.set || (!after && !t.hasBefore)) continue;
                const StrokeField field = static_cast<StrokeField>(i);
                sets.push_back({fn.findPlug(strokeFieldName(field), true), after ? t.after : t.before, field == StrokeField::Graph});
            }
        }
        // A graph that would not load is refused before anything is set.
        for (const Set& set : sets) {
            if (!set.graph) continue;
            try {
                GroomSession probe;
                probe.setGraphJson(set.text);
            } catch (const std::exception& e) {
                return e.what();
            }
        }
        for (const Set& set : sets) cmd->modifier().newPlugValueString(set.plug, set.text.c_str());
        if (!cmd->modifier().doIt()) return "Maya would not set the groom's data";
        cmd->touched();
        shape()->ensureEvaluated();
        if (MGlobal::mayaState() == MGlobal::kInteractive) M3dView::active3dView().refresh(false, true);
        return {};
    }

    SimRun simulateTo(int toFrame) override { return shape()->simulateTo(toFrame); }
    void setCollider(const std::string&, MeshPtr) override {
        throw ToolError("in Maya, colliders are meshes of the scene: connect one to the groom's colliders "
                        "(GroomLab > Simulate), then name it in a ColliderMesh node");
    }
    CardBake bakeCards(int resolution, float occlusion) override {
        try {
            return shape()->bakeCardsInMemory(resolution, occlusion);
        } catch (const std::runtime_error& e) {
            throw ToolError(e.what());
        }
    }

private:
    MObjectHandle node_;
    ViewResult view_;
};

// The scene's grooms, each named by its transform (the groom the artist sees in the Outliner).
class MayaTarget : public agent::AgentTarget {
public:
    agent::AgentFolders folders_;

    const agent::AgentFolders& folders() const override { return folders_; }

    std::vector<AgentGroom*> grooms() override {
        std::map<std::string, std::unique_ptr<MayaGroom>> now;
        const double frame = MAnimControl::currentTime().as(MTime::uiUnit());
        for (MItDependencyNodes it(MFn::kPluginShape); !it.isDone(); it.next()) {
            MFnDependencyNode node(it.thisNode());
            if (node.typeId() != GroomShape::kTypeId) continue;
            const std::string id = node.uuid().asString().asChar();
            MFnDagNode dag(it.thisNode());
            std::string name = node.name().asChar();
            if (dag.parentCount() > 0) name = MFnDependencyNode(dag.parent(0)).name().asChar();
            auto found = grooms_.find(id);
            std::unique_ptr<MayaGroom> g;
            if (found != grooms_.end() && found->second->alive() && found->second->name() == name) {
                g = std::move(found->second);
            } else {
                g = std::make_unique<MayaGroom>(name, it.thisNode());
            }
            g->frame = frame;
            now[id] = std::move(g);
        }
        grooms_ = std::move(now);
        std::vector<AgentGroom*> out;
        for (auto& [id, g] : grooms_) out.push_back(g.get());
        std::sort(out.begin(), out.end(), [](AgentGroom* a, AgentGroom* b) { return a->name() < b->name(); });
        return out;
    }

    // "": the groom GroomLab's tools work on (the current or selected one); a name: the
    // transform's or the shape's.
    AgentGroom& groom(const std::string& name) override {
        const std::vector<AgentGroom*> all = grooms();
        if (all.empty()) throw ToolError("the scene has no groom: make one (GroomLab > Create Groom on a selected mesh)");
        if (name.empty()) {
            MDagPath active;
            if (activeGroom(active)) {
                const std::string id = MFnDependencyNode(active.node()).uuid().asString().asChar();
                if (grooms_.count(id)) return *grooms_[id];
            }
            if (all.size() == 1) return *all.front();
            std::string names;
            for (const AgentGroom* g : all) names += (names.empty() ? "" : ", ") + g->name();
            throw ToolError("several grooms are in the scene (" + names + ") and none is current: say which with \"groom\"");
        }
        for (auto& [id, g] : grooms_) {
            if (g->name() == name || std::string(MFnDependencyNode(g->node()).name().asChar()) == name) return *g;
        }
        return AgentTarget::groom(name);  // (says which grooms there are)
    }

    std::string opensNoGrooms() const override {
        return "in Maya the agent works on the scene's grooms (list_grooms): make one with GroomLab > Create Groom from "
               "Selection, or import a groom file with GroomLab > Import";
    }
    AgentGroom& open(const std::string&, GroomFile) override { throw ToolError(opensNoGrooms()); }
    void close(const std::string&) override {}
    std::shared_ptr<const ScalpMesh> readMesh(const std::string&, const std::string&) override {
        throw ToolError("in Maya, meshes are the scene's: import or create the mesh in Maya");
    }
    agent::ExportReport exportCurves(AgentGroom&, const agent::ExportRequest&) override {
        throw ToolError("in Maya, export the curves with GroomLab > Export (Alembic, USD, V-Ray); save_groom writes a .groom file");
    }

    // The active viewport as the artist sees it, read back after a refresh.
    std::optional<agent::PreviewImage> viewportImage(AgentGroom&, int size) override {
        if (MGlobal::mayaState() != MGlobal::kInteractive) return std::nullopt;
        M3dView view = M3dView::active3dView();
        view.refresh(false, true);
        MImage image;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // (deprecated for render targets; still what reads the view as shown)
#endif
        const bool read = view.readColorBuffer(image, true);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (!read) return std::nullopt;
        unsigned w = 0, h = 0;
        image.getSize(w, h);
        const unsigned char* pixels = image.pixels();
        if (!pixels || w == 0 || h == 0) return std::nullopt;
        const double k = std::min(1.0, static_cast<double>(size) / static_cast<double>(std::max(w, h)));
        const int ow = std::max(1, static_cast<int>(w * k)), oh = std::max(1, static_cast<int>(h * k));
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh) * 3);
        for (int y = 0; y < oh; ++y) {
            const unsigned sy = std::min(h - 1, static_cast<unsigned>((oh - 1 - y) / k));  // (the buffer's rows bottom up)
            for (int x = 0; x < ow; ++x) {
                const unsigned sx = std::min(w - 1, static_cast<unsigned>(x / k));
                const unsigned char* p = pixels + (static_cast<std::size_t>(sy) * w + sx) * 4;
                std::uint8_t* q = rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)) * 3;
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
            }
        }
        agent::PreviewImage out;
        out.width = ow;
        out.height = oh;
        out.png = encodePng(ow, oh, 3, rgb);
        return out;
    }

private:
    std::map<std::string, std::unique_ptr<MayaGroom>> grooms_;
};

struct Link {
    std::unique_ptr<MayaTarget> target;
    std::unique_ptr<agent::AgentTools> tools;
    std::string token, file;
    int port = 0;
};
Link& link() {
    static Link l;
    return l;
}

agent::AgentTools& tools() {
    Link& l = link();
    if (!l.tools) {
        l.target = std::make_unique<MayaTarget>();
        l.tools = std::make_unique<agent::AgentTools>(*l.target);
    }
    return *l.tools;
}

std::string activeGroomName() {
    MDagPath path;
    if (!activeGroom(path)) return {};
    MDagPath transform = path;
    transform.pop();
    return transform.partialPathName().asChar();
}

long long ownPid() {
#ifdef _WIN32
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

}  // namespace

const char* AgentCmd::kName = "groomLabAgent";

void* AgentCmd::creator() { return new AgentCmd(); }

MSyntax AgentCmd::newSyntax() {
    MSyntax syntax;
    syntax.addFlag("-st", "-start");
    syntax.addFlag("-p", "-port", MSyntax::kLong);
    syntax.addFlag("-fd", "-folders", MSyntax::kString);
    syntax.addFlag("-sp", "-stop");
    syntax.addFlag("-sts", "-status");
    syntax.addFlag("-rq", "-request", MSyntax::kString);
    syntax.addFlag("-gn", "-groomName");
    syntax.enableQuery(false);
    syntax.enableEdit(false);
    return syntax;
}

AgentCmd* AgentCmd::current() { return gCurrent; }

void AgentCmd::shutdown() {
    Link& l = link();
    if (!l.file.empty()) agent::removeLinkInfo(l.file);
    l.file.clear();
    l.token.clear();
    l.port = 0;
    l.tools.reset();
    l.target.reset();
}

MStatus AgentCmd::doIt(const MArgList& args) {
    return guarded("AgentCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        Link& l = link();
        if (db.isFlagSet("-st")) {
            int port = 0;
            if (!db.isFlagSet("-p") || !db.getFlagArgument("-p", 0, port) || port <= 0 || port > 65535) {
                return fail(kName, "-start needs the -port the listener is on");
            }
            MayaTarget& target = (tools(), *l.target);
            if (db.isFlagSet("-fd")) {
                const json f = json::parse(flagString(db, "-fd").asChar(), nullptr, false);
                if (!f.is_object()) return fail(kName, "-folders is a JSON object");
                // (canonical: paths an agent gives are compared with it once resolved)
                std::error_code ec;
                const std::filesystem::path root = std::filesystem::u8path(f.value("root", std::string()));
                const std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(root, ec), ec);
                target.folders_.root = (ec ? root : canonical).u8string();
                target.folders_.presetDir = f.value("presets", std::string());
                target.folders_.meshDir = f.value("meshes", std::string());
                target.folders_.docsDir = f.value("docs", std::string());
            }
            if (!l.file.empty()) agent::removeLinkInfo(l.file);
            agent::LinkInfo info;
            info.host = "maya";
            info.pid = ownPid();
            info.port = port;
            info.token = agent::newLinkToken();
            info.groom = activeGroomName();
            info.started = agent::linkClock();
            info.version = kVersionString;
            l.file = agent::writeLinkInfo(info);
            l.token = info.token;
            l.port = port;
            setResult(json{{"token", l.token}, {"file", l.file}}.dump().c_str());
            return MS::kSuccess;
        }
        if (db.isFlagSet("-sp")) {
            const bool was = !l.file.empty();
            shutdown();
            setResult(was);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-sts")) {
            setResult(json{{"running", !l.file.empty()}, {"port", l.port}, {"file", l.file}, {"groom", activeGroomName()}}.dump().c_str());
            return MS::kSuccess;
        }
        if (db.isFlagSet("-gn")) {
            setResult(activeGroomName().c_str());
            return MS::kSuccess;
        }
        if (db.isFlagSet("-rq")) {
            if (l.token.empty()) return fail(kName, "the agent link is off (groomLabAgent -start)");
            const std::string line = flagString(db, "-rq").asChar();
            gCurrent = this;
            std::string answer;
            try {
                answer = agent::answerLinkRequest(tools(), l.token, line);
            } catch (...) {
                gCurrent = nullptr;
                throw;
            }
            gCurrent = nullptr;
            setResult(answer.c_str());
            return MS::kSuccess;
        }
        return fail(kName, "give -start, -stop, -status, -groomName or -request");
    });
}

MStatus AgentCmd::redoIt() {
    return guarded("AgentCmd::redoIt", [&]() -> MStatus {
        const MStatus status = modifier_.doIt();
        if (status && MGlobal::mayaState() == MGlobal::kInteractive) M3dView::active3dView().refresh(false, true);
        return status;
    });
}

MStatus AgentCmd::undoIt() {
    return guarded("AgentCmd::undoIt", [&]() -> MStatus {
        const MStatus status = modifier_.undoIt();
        if (status && MGlobal::mayaState() == MGlobal::kInteractive) M3dView::active3dView().refresh(false, true);
        return status;
    });
}

}  // namespace groomlab::maya
