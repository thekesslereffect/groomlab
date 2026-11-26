// SPDX-License-Identifier: Apache-2.0
// The agent link in the app (agent_link.h): Groom Tools > More > Agent Link starts the listener and
// writes the discovery file; an agent's client (a socket here, as groomlab-cli mcp --live talks:
// JSON lines with the file's token) lists the tools, places and combs guides on the open groom,
// each one step of the document's history; the agent's undo and Edit > Undo are one stack; the
// header's badge shows while it is connected and its Stop turns the link off.

#include <filesystem>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QToolButton>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agent_link.h"
#include "app_actions.h"
#include "definitions.h"
#include "groom_document.h"
#include "groom_tools.h"
#include "groomlab/editor/agent_link.h"
#include "main_window.h"

using nlohmann::json;
using namespace groomlab::app;
namespace fs = std::filesystem;

namespace {

// One request answered: the line out, events run (the window's server answers on them) until a
// line comes back.
json ask(QTcpSocket& socket, const json& request) {
    const std::string line = request.dump() + "\n";
    socket.write(line.data(), static_cast<qint64>(line.size()));
    socket.flush();
    QElapsedTimer clock;
    clock.start();
    while (!socket.canReadLine() && clock.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!socket.canReadLine()) return json{{"error", {{"message", "no answer"}}}};
    return json::parse(socket.readLine().toStdString());
}

int storedGuides(GroomDocument& doc) {
    const std::string& text = doc.session().data().hierarchyJson;
    return text.empty() ? 0 : static_cast<int>(json::parse(text)["guides"].size());
}

json textOf(const json& answer) {
    const json& content = answer["result"]["content"];
    for (auto it = content.rbegin(); it != content.rend(); ++it) {
        if ((*it)["type"] == "text") return json::parse((*it)["text"].get<std::string>());
    }
    return json();
}

}  // namespace

TEST(AppAgentLink, AnAgentGroomsTheOpenGroomInTheDocumentsHistory) {
    const fs::path links = fs::temp_directory_path() / "groomlab_app_agent_link";
    fs::remove_all(links);
    qputenv("GROOMLAB_AGENT_LINK_DIR", QByteArray::fromStdString(links.u8string()));
    MainWindow window;
    window.resize(1280, 800);
    ASSERT_TRUE(window.newGroomOnMesh(MainWindow::sampleMeshFile(QStringLiteral("nika"))));
    GroomDocument* doc = window.document();
    ASSERT_TRUE(doc);
    GroomTools* tools = window.findChild<GroomTools*>();
    ASSERT_TRUE(tools);
    EXPECT_FALSE(tools->agentBadge()->isVisibleTo(tools));

    // More > Agent Link: on, its file written (host "app", this process).
    const ui::Bindings bindings = AppActions::bindings(window);
    ASSERT_TRUE(bindings.count("agent.link"));
    EXPECT_FALSE(bindings.at("agent.link").checked(json()));
    bindings.at("agent.link").trigger(json(), true);
    ASSERT_TRUE(window.agentLink()->running());
    EXPECT_TRUE(bindings.at("agent.link").checked(json()));
    const std::vector<groomlab::agent::LinkInfo> live = groomlab::agent::liveLinks("app", links.u8string());
    ASSERT_EQ(live.size(), 1u);
    const groomlab::agent::LinkInfo link = live.front();
    EXPECT_EQ(link.pid, QCoreApplication::applicationPid());
    EXPECT_EQ(link.port, window.agentLink()->port());

    // An agent connects: the badge shows.
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(link.port));
    ASSERT_TRUE(socket.waitForConnected(10000));
    int id = 1;
    auto request = [&](const std::string& method, const json& params, const std::string& token) {
        return ask(socket, {{"token", token}, {"id", id++}, {"method", method}, {"params", params}});
    };
    auto call = [&](const std::string& name, const json& arguments) {
        return request("tools/call", {{"name", name}, {"arguments", arguments}}, link.token);
    };
    const json listed = request("tools/list", json::object(), link.token);
    ASSERT_TRUE(listed.contains("result")) << listed.dump();
    EXPECT_GT(listed["result"]["tools"].size(), 40u);
    EXPECT_TRUE(window.agentLink()->clients() == 1 && tools->agentBadge()->isVisibleTo(tools));
    EXPECT_EQ(request("tools/list", json::object(), "wrong")["error"]["code"], groomlab::agent::kLinkBadToken);

    // Guides placed and combed: steps of the document's history, its stored guides.
    const std::size_t steps = doc->history().size();
    const json placed = call("brush_stroke", {{"mode", "place"}, {"from", "front_hairline"}, {"via", {"top"}}, {"to", "crown"},
                                              {"length", 12}, {"match_neighbors", false}});
    ASSERT_EQ(placed["result"]["isError"], false) << placed.dump();
    const int made = textOf(placed)["changed"]["guidesAdded"].get<int>();
    EXPECT_GT(made, 2);
    EXPECT_EQ(storedGuides(*doc), made);
    EXPECT_EQ(doc->history().size(), steps + 1);
    EXPECT_EQ(doc->undoText(), "Undo place stroke");
    const std::string placedText = doc->session().data().hierarchyJson;
    const json combed = call("brush_stroke", {{"mode", "comb"}, {"from", "crown"}, {"to", "back"}, {"offset", 3}, {"radius", 6}});
    ASSERT_EQ(combed["result"]["isError"], false) << combed.dump();
    EXPECT_GT(textOf(combed)["changed"]["guidesTouched"].get<int>(), 0);
    const std::string combedText = doc->session().data().hierarchyJson;
    EXPECT_NE(combedText, placedText);

    // The agent's undo is the document's: the comb taken back; Edit > Undo takes the guides.
    const json undone = call("undo", json::object());
    EXPECT_EQ(textOf(undone)["undone"], json::array({"comb stroke"}));
    EXPECT_EQ(doc->session().data().hierarchyJson, placedText);
    bindings.at("edit.undo").trigger(json(), false);
    EXPECT_EQ(storedGuides(*doc), 0);
    bindings.at("edit.redo").trigger(json(), false);
    const json redone = call("redo", json::object());
    EXPECT_EQ(textOf(redone)["redone"], json::array({"comb stroke"}));
    EXPECT_EQ(doc->session().data().hierarchyJson, combedText);
    // A graph edit lands in the document's graph (what the Graph Editor and saving read).
    const json added = call("add_node", {{"type", "Curl"}});
    ASSERT_EQ(added["result"]["isError"], false) << added.dump();
    const std::string curl = textOf(added)["node"].get<std::string>();
    auto hasNode = [&](const std::string& name) {
        for (const GroomDocument::NodeInfo& n : doc->nodes()) {
            if (n.name == name) return true;
        }
        return false;
    };
    EXPECT_TRUE(hasNode(curl));
    call("undo", json::object());
    EXPECT_FALSE(hasNode(curl));

    // The viewport as the artist sees it.
    const json shot = call("render_preview", {{"viewport", true}, {"size", 128}});
    ASSERT_EQ(shot["result"]["isError"], false) << shot.dump().substr(0, 500);
    EXPECT_EQ(shot["result"]["content"][0]["type"], "image");

    // The badge's Stop: the link off, its file gone, the badge hidden.
    tools->agentBadge()->findChild<QToolButton*>(QStringLiteral("agentStop"))->click();
    EXPECT_FALSE(window.agentLink()->running());
    EXPECT_TRUE(groomlab::agent::liveLinks("app", links.u8string()).empty());
    QCoreApplication::processEvents();
    EXPECT_FALSE(tools->agentBadge()->isVisibleTo(tools));
    qunsetenv("GROOMLAB_AGENT_LINK_DIR");
}
