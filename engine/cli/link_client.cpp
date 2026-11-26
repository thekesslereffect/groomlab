// SPDX-License-Identifier: Apache-2.0
#include "link_client.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace groomlab::cli {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
struct Winsock {
    Winsock() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void closeSocket(SocketHandle s) { close(s); }
#endif

}  // namespace

struct LinkClient::Socket {
#ifdef _WIN32
    Winsock winsock;
#endif
    SocketHandle handle = kNoSocket;
    ~Socket() {
        if (handle != kNoSocket) closeSocket(handle);
    }
};

LinkClient::LinkClient(agent::LinkInfo info) : info_(std::move(info)), socket_(std::make_unique<Socket>()) {
#ifdef _WIN32
    if (!socket_->winsock.ok) throw LinkLost("cannot start Windows sockets");
#endif
    socket_->handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_->handle == kNoSocket) throw LinkLost("cannot make a socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(info_.port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never another machine
    if (::connect(socket_->handle, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        throw LinkLost("cannot connect to " + info_.host + " (pid " + std::to_string(info_.pid) + ") on port " +
                       std::to_string(info_.port));
    }
    int on = 1;
    setsockopt(socket_->handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

LinkClient::~LinkClient() = default;

nlohmann::json LinkClient::request(const std::string& method, const nlohmann::json& params) {
    const std::int64_t id = nextId_++;
    const std::string line = agent::linkRequestLine(info_.token, id, method, params) + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
        const auto n = ::send(socket_->handle, line.data() + sent, static_cast<int>(line.size() - sent), 0);
        if (n <= 0) throw LinkLost(info_.host + " closed the link");
        sent += static_cast<std::size_t>(n);
    }
    // One line back (long ones: an image's base64).
    char chunk[65536];
    for (;;) {
        const auto end = buffer_.find('\n');
        if (end != std::string::npos) {
            std::string answer = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            if (!answer.empty() && answer.back() == '\r') answer.pop_back();
            return agent::linkResult(answer, id);
        }
        if (buffer_.size() > agent::kMaxLinkLine) throw LinkLost("the host's answer is too long");
        const auto n = ::recv(socket_->handle, chunk, static_cast<int>(sizeof chunk), 0);
        if (n <= 0) throw LinkLost(info_.host + " closed the link");
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

}  // namespace groomlab::cli
