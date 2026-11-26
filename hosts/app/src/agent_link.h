// SPDX-License-Identifier: Apache-2.0
#pragma once

// The agent link in the app (engine/editor agent_link.h, agent_tools.h): while Groom Tools > More >
// Agent Link is on, an AI agent's MCP server (groomlab-cli mcp --live) reaches the open groom. A
// QTcpServer on 127.0.0.1 on the main thread answers each request line with answerLinkRequest
// against the app's target: the open document, its edits recorded in the document's own history
// (GroomDocument::commitAgentEdit), so Edit > Undo and the agent's undo are one stack. The
// discovery file (host "app") is written while the link is on and removed when it stops or the
// window goes.

#include <memory>
#include <string>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace groomlab::agent {
class AgentTools;
}

namespace groomlab::app {

class MainWindow;
class AppAgentTarget;

class AppAgentLink : public QObject {
    Q_OBJECT
public:
    explicit AppAgentLink(MainWindow& window);
    ~AppAgentLink() override;

    // The link on (a new token and file); false and *error when it cannot listen or write its file.
    bool start(QString* error = nullptr);
    void stop();
    bool running() const { return server_ != nullptr; }
    int clients() const { return static_cast<int>(sockets_.size()); }
    QString file() const { return QString::fromStdString(file_); }
    int port() const { return port_; }

    // One request line answered (the answer line): what a connection's lines go through.
    std::string answer(const std::string& line);

signals:
    // Started, stopped, an agent connected or left.
    void changed();

private:
    void accept();
    void read(QTcpSocket* socket);
    void gone(QTcpSocket* socket);

    MainWindow& window_;
    QTcpServer* server_ = nullptr;
    QList<QTcpSocket*> sockets_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    std::unique_ptr<AppAgentTarget> target_;
    std::unique_ptr<groomlab::agent::AgentTools> tools_;
    std::string token_, file_;
    int port_ = 0;
};

}  // namespace groomlab::app
