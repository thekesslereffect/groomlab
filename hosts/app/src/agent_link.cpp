// SPDX-License-Identifier: Apache-2.0
#include "agent_link.h"

#include <cstring>
#include <filesystem>
#include <optional>

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

#include <nlohmann/json.hpp>

#include "graph_presets.h"
#include "groom_document.h"
#include "groomlab/editor/agent_link.h"
#include "groomlab/editor/agent_tools.h"
#include "groomlab/png.h"
#include "groomlab/version.h"
#include "main_window.h"
#include "viewport.h"

namespace groomlab::app {

using agent::ToolError;
namespace fs = std::filesystem;

// The open document as an agent's groom: its session read, its edits recorded in its own history.
class AppAgentGroom : public agent::AgentGroom {
public:
    AppAgentGroom(std::string name, GroomDocument& doc, MainWindow& window) : AgentGroom(std::move(name)), doc_(doc), window_(window) {}
    GroomDocument& document() { return doc_; }

    GroomSession& session() override { return doc_.session(); }
    std::string upAxis() const override { return doc_.file().upAxis; }
    std::string units() const override { return doc_.file().units; }
    double framesPerSecond() const override { return doc_.framesPerSecond(); }
    const ViewResult& view(const ViewRequest& request) override { return doc_.evaluate(request); }
    GroomFile toFile() override { return doc_.currentFile(); }

    void commit(GroomEdit edit) override {
        const std::string error = doc_.commitAgentEdit(std::move(edit));
        if (!error.empty()) throw ToolError("the edit was refused, so nothing changed: " + error);
    }
    EditHistory& history() override { return doc_.history(); }
    std::string undo() override { return doc_.undo(); }
    std::string redo() override { return doc_.redo(); }
    SimRun simulateTo(int toFrame) override { return doc_.simulateTo(toFrame); }
    // Morph to Photos as the Photos tab does it: the groom carried onto the morphed scalp (a
    // transfer: one step of the document's history).
    MorphOutcome morphToPhotos(const MorphRequest& request) override {
        QString error;
        if (!window_.morphScalpToPhotos(request.stiffness, request.symmetric, request.reset, &error)) throw ToolError(error.toStdString());
        MorphOutcome out;
        out.undoable = false;
        out.note = request.reset ? "the groom carried back onto its unmorphed scalp"
                                 : "the groom carried onto the morphed scalp, as the Photos tab's Morph does: a transfer, which starts the "
                                   "app's history over (morph_to_photos reset brings the unmorphed scalp back)";
        return out;
    }
    void setCollider(const std::string&, MeshPtr) override {
        throw ToolError("in the app, colliders are props: add one with File > Add Prop, then name it in a ColliderMesh node");
    }
    CardBake bakeCards(int resolution, float occlusion) override {
        try {
            return doc_.bakeCardsInMemory(resolution, occlusion);
        } catch (const std::runtime_error& e) {
            throw ToolError(e.what());
        }
    }

private:
    GroomDocument& doc_;
    MainWindow& window_;
};

// The app's one groom: the document open now.
class AppAgentTarget : public agent::AgentTarget {
public:
    explicit AppAgentTarget(MainWindow& window) : window_(window) {}

    agent::AgentFolders folders_;
    const agent::AgentFolders& folders() const override { return folders_; }

    std::vector<agent::AgentGroom*> grooms() override {
        GroomDocument* doc = window_.document();
        if (!doc) {
            groom_.reset();
            return {};
        }
        if (!groom_ || &groom_->document() != doc) {
            const std::string name = doc->path().empty() ? std::string("untitled") : fs::u8path(doc->path()).stem().u8string();
            groom_ = std::make_unique<AppAgentGroom>(name, *doc, window_);
        }
        // (the photos as the document has them now: the Reference window edits them too)
        groom_->referencePhotos = doc->referencePhotos();
        groom_->frame = doc->time() * doc->framesPerSecond();
        return {groom_.get()};
    }
    agent::AgentGroom& groom(const std::string& name) override {
        const std::vector<agent::AgentGroom*> all = grooms();
        if (all.empty()) throw ToolError("no groom is open in the app: open one (File > Open) or make one (New Groom on Mesh)");
        if (name.empty() || name == all.front()->name()) return *all.front();
        throw ToolError("the app has one groom open, '" + all.front()->name() + "': leave \"groom\" out or name it");
    }
    std::string opensNoGrooms() const override {
        return "in the app the agent works on the open groom: open one with File > Open, or make one with New Groom on Mesh";
    }
    agent::AgentGroom& open(const std::string&, GroomFile) override { throw ToolError(opensNoGrooms()); }
    void close(const std::string&) override {}
    std::shared_ptr<const ScalpMesh> readMesh(const std::string&, const std::string&) override {
        throw ToolError("in the app, meshes come in with New Groom on Mesh or File > Add Prop");
    }
    agent::ExportReport exportCurves(agent::AgentGroom&, const agent::ExportRequest&) override {
        throw ToolError("in the app, export the curves with File > Export (Alembic, USD, V-Ray); save_groom writes a .groom file");
    }
    // The lookdev render: the hair's Arnold look under the viewport's lights, through its camera.
    std::vector<agent::RendererInfo> renderers() override {
        return {{"lookdev", "the app's lookdev render (the hair's Arnold look, the viewport's lights and camera), offscreen on the GPU"}};
    }
    agent::RenderedFrame render(agent::AgentGroom&, const agent::RenderRequest& r) override {
        if (!r.camera.empty()) throw ToolError("the app renders through its viewport's camera: leave \"camera\" out");
        if (r.frame) throw ToolError("the app renders the frame shown: set the time in the app, or leave \"frame\" out");
        // Frames accumulated (the lookdev's refinement) by quality, 4 samples a pixel.
        const int frames = r.samples > 0 ? std::min(r.samples, 256) : r.quality == "final" ? 64 : r.quality == "preview" ? 16 : 4;
        QElapsedTimer clock;
        clock.start();
        QString api;
        const QImage image = window_.renderLookdev(QSize(r.width, r.height), frames, 4, &api).convertToFormat(QImage::Format_RGB888);
        if (image.isNull()) throw ToolError("the GPU gave no pixels for the lookdev render");
        agent::RenderedFrame out;
        out.image = pngOf(image);
        out.seconds = static_cast<double>(clock.elapsed()) / 1000.0;
        out.info = {{"frames", frames}, {"api", api.toStdString()}};
        return out;
    }

    // The viewport as the artist sees it: the window's scene rendered offscreen at the view's shape.
    std::optional<agent::PreviewImage> viewportImage(agent::AgentGroom&, int size) override {
        QSize shape(16, 9);
        if (Viewport* view = window_.viewport(); view && view->width() > 0 && view->height() > 0) shape = view->size();
        const double k = static_cast<double>(size) / std::max(shape.width(), shape.height());
        const QSize at(std::max(16, static_cast<int>(shape.width() * k)), std::max(16, static_cast<int>(shape.height() * k)));
        const QImage image = window_.renderOffscreen(at, 1).convertToFormat(QImage::Format_RGB888);
        if (image.isNull()) return std::nullopt;
        return pngOf(image);
    }

    static agent::PreviewImage pngOf(const QImage& image) {
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height()) * 3);
        for (int y = 0; y < image.height(); ++y) {
            std::memcpy(rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) * 3, image.constScanLine(y),
                        static_cast<std::size_t>(image.width()) * 3);
        }
        agent::PreviewImage out;
        out.width = image.width();
        out.height = image.height();
        out.png = encodePng(out.width, out.height, 3, rgb);
        return out;
    }

private:
    MainWindow& window_;
    std::unique_ptr<AppAgentGroom> groom_;
};

AppAgentLink::AppAgentLink(MainWindow& window) : QObject(&window), window_(window) {}

AppAgentLink::~AppAgentLink() { stop(); }

std::string AppAgentLink::root() const { return target_ ? target_->folders_.root : std::string(); }

bool AppAgentLink::start(QString* error) {
    stop();
    auto* server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        if (error) *error = tr("cannot listen on 127.0.0.1: %1").arg(server->errorString());
        delete server;
        return false;
    }
    if (!target_) {
        target_ = std::make_unique<AppAgentTarget>(window_);
        tools_ = std::make_unique<agent::AgentTools>(*target_);
    }
    // The files the tools read and write: the open groom's folder, else Documents/GroomLab.
    GroomDocument* doc = window_.document();
    std::string root = doc && !doc->path().empty() ? fs::u8path(doc->path()).parent_path().u8string() : std::string();
    if (root.empty()) {
        root = (fs::u8path(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdString()) / "GroomLab").u8string();
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(root), ec);
    target_->folders_.root = fs::weakly_canonical(fs::u8path(root), ec).u8string();
    target_->folders_.presetDir = shippedPresetDir();
    target_->folders_.meshDir = shippedMeshFolder();
    agent::LinkInfo info;
    info.host = "app";
    info.pid = static_cast<long long>(QCoreApplication::applicationPid());
    info.port = server->serverPort();
    info.token = agent::newLinkToken();
    info.groom = doc && !doc->path().empty() ? fs::u8path(doc->path()).stem().u8string() : std::string();
    info.started = agent::linkClock();
    info.version = kVersionString;
    try {
        file_ = agent::writeLinkInfo(info);
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        delete server;
        return false;
    }
    token_ = info.token;
    port_ = info.port;
    server_ = server;
    connect(server_, &QTcpServer::newConnection, this, &AppAgentLink::accept);
    emit changed();
    return true;
}

void AppAgentLink::stop() {
    if (!server_) return;
    const QList<QTcpSocket*> sockets = sockets_;
    sockets_.clear();
    buffers_.clear();
    for (QTcpSocket* s : sockets) {
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    server_->close();
    server_->deleteLater();
    server_ = nullptr;
    agent::removeLinkInfo(file_);
    file_.clear();
    token_.clear();
    port_ = 0;
    emit changed();
}

void AppAgentLink::accept() {
    while (server_ && server_->hasPendingConnections()) {
        QTcpSocket* s = server_->nextPendingConnection();
        sockets_.push_back(s);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { read(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] { gone(s); });
        emit changed();
    }
}

void AppAgentLink::gone(QTcpSocket* s) {
    if (!sockets_.removeOne(s)) return;
    buffers_.remove(s);
    s->deleteLater();
    emit changed();
}

void AppAgentLink::read(QTcpSocket* s) {
    QByteArray& buffer = buffers_[s];
    buffer += s->readAll();
    for (qsizetype end = buffer.indexOf('\n'); end >= 0; end = buffer.indexOf('\n')) {
        const QByteArray line = buffer.left(end).trimmed();
        buffer.remove(0, end + 1);
        if (line.isEmpty()) continue;
        const std::string reply = answer(line.toStdString()) + "\n";
        s->write(reply.data(), static_cast<qint64>(reply.size()));
        s->flush();
        if (!sockets_.contains(s)) return;  // (the answer stopped the link)
    }
    if (buffer.size() > static_cast<qsizetype>(agent::kMaxLinkLine)) {
        s->abort();
    }
}

std::string AppAgentLink::answer(const std::string& line) {
    if (!tools_ || token_.empty()) return R"({"id": null, "error": {"code": -32603, "message": "the agent link is off"}})";
    GroomDocument* doc = window_.document();
    const std::size_t steps = doc ? doc->history().size() : 0, at = doc ? doc->undoIndex() : 0;
    const std::string reply = agent::answerLinkRequest(*tools_, token_, line);
    // What the agent changed shows at once, as an undo or a panel's edit does.
    if (window_.document() && (window_.document() != doc || window_.document()->history().size() != steps ||
                               window_.document()->undoIndex() != at)) {
        window_.agentEdited();
    }
    return reply;
}

}  // namespace groomlab::app
