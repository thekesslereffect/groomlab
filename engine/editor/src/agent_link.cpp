// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_link.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "groomlab/editor/agent_tools.h"
#include "groomlab/paths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

namespace groomlab::agent {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

json errorAnswer(const json& id, int code, const std::string& message) {
    return {{"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::string dumpLine(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace

json LinkInfo::toJson() const {
    return {{"host", host}, {"pid", pid}, {"port", port}, {"token", token}, {"groom", groom}, {"started", started}, {"version", version}};
}

LinkInfo LinkInfo::fromJson(const json& j) {
    if (!j.is_object() || !j.contains("host") || !j.contains("pid") || !j.contains("port") || !j.contains("token")) {
        throw std::invalid_argument("not an agent link file");
    }
    LinkInfo info;
    try {
        info.host = j.at("host").get<std::string>();
        info.pid = j.at("pid").get<long long>();
        info.port = j.at("port").get<int>();
        info.token = j.at("token").get<std::string>();
        info.groom = j.value("groom", std::string());
        info.started = j.value("started", 0.0);
        info.version = j.value("version", std::string());
    } catch (const json::exception& e) {
        throw std::invalid_argument(std::string("a bad agent link file: ") + e.what());
    }
    if (info.port <= 0 || info.port > 65535 || info.token.empty()) throw std::invalid_argument("a bad agent link file: port or token");
    return info;
}

std::string agentLinkDir() {
    if (const std::optional<std::string> own = environmentVariable("GROOMLAB_AGENT_LINK_DIR"); own && !own->empty()) return *own;
#ifdef _WIN32
    const std::optional<std::string> home = environmentVariable("USERPROFILE");
#else
    const std::optional<std::string> home = environmentVariable("HOME");
#endif
    if (!home || home->empty()) return {};
    return (fs::u8path(*home) / "Documents" / "GroomLab" / "agent_link").generic_u8string();
}

std::string newLinkToken() {
    std::random_device device;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i) {
        out << std::hex;
        out.width(8);
        out.fill('0');
        out << static_cast<std::uint32_t>(device());
    }
    return out.str();
}

double linkClock() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool processAlive(long long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;  // (there, but not ours to look at)
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string writeLinkInfo(const LinkInfo& info, const std::string& dir) {
    if (dir.empty()) throw std::runtime_error("no folder for the agent link's file (no home folder)");
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    const fs::path path = fs::u8path(dir) / fs::u8path(info.host + "-" + std::to_string(info.pid) + ".json");
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + temp.u8string());
        out << info.toJson().dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + temp.u8string());
    }
    fs::rename(temp, path, ec);
    if (ec) throw std::runtime_error("cannot write " + path.u8string() + ": " + ec.message());
    return path.u8string();
}

void removeLinkInfo(const std::string& path) {
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
}

std::vector<LinkInfo> liveLinks(const std::string& host, const std::string& dir) {
    std::vector<LinkInfo> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::u8path(dir), ec)) return out;
    std::vector<fs::path> stale;
    for (const auto& entry : fs::directory_iterator(fs::u8path(dir), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        LinkInfo info;
        try {
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            info = LinkInfo::fromJson(json::parse(text.str()));
        } catch (const std::exception&) {
            stale.push_back(entry.path());
            continue;
        }
        if (!processAlive(info.pid)) {
            stale.push_back(entry.path());
            continue;
        }
        info.file = entry.path().u8string();
        if (host.empty() || info.host == host) out.push_back(std::move(info));
    }
    for (const fs::path& p : stale) fs::remove(p, ec);
    std::sort(out.begin(), out.end(), [](const LinkInfo& a, const LinkInfo& b) {
        return a.started != b.started ? a.started > b.started : a.pid > b.pid;
    });
    return out;
}

std::string answerLinkRequest(AgentTools& tools, const std::string& token, const std::string& line) {
    if (line.size() > kMaxLinkLine) return dumpLine(errorAnswer(nullptr, kLinkBadRequest, "the request is too long"));
    const json request = json::parse(line, nullptr, false);
    if (!request.is_object()) return dumpLine(errorAnswer(nullptr, kLinkBadRequest, "a request is a JSON object on one line"));
    const json id = request.contains("id") ? request["id"] : json(nullptr);
    if (token.empty() || !request.contains("token") || !request["token"].is_string() || request["token"].get<std::string>() != token) {
        return dumpLine(errorAnswer(id, kLinkBadToken, "wrong or missing token: read it from the host's agent link file"));
    }
    const std::string method = request.value("method", std::string());
    const json params = request.value("params", json::object());
    try {
        if (method == "ping") return dumpLine({{"id", id}, {"result", json::object()}});
        if (method == "tools/list") return dumpLine({{"id", id}, {"result", {{"tools", tools.list()}}}});
        if (method == "tools/call") {
            if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
                return dumpLine(errorAnswer(id, kLinkBadRequest, "tools/call needs a tool \"name\""));
            }
            const std::string name = params["name"].get<std::string>();
            if (!tools.has(name)) return dumpLine(errorAnswer(id, kLinkBadRequest, "unknown tool '" + name + "'"));
            const json arguments = params.value("arguments", json::object());
            const ToolResult result = tools.call(name, arguments.is_object() ? arguments : json::object());
            return dumpLine({{"id", id}, {"result", {{"content", result.content}, {"isError", result.isError}}}});
        }
    } catch (const std::exception& e) {
        return dumpLine(errorAnswer(id, -32603, e.what()));
    }
    return dumpLine(errorAnswer(id, kLinkUnknownMethod, "unknown method '" + method + "'"));
}

std::string linkRequestLine(const std::string& token, std::int64_t id, const std::string& method, const json& params) {
    json request = {{"token", token}, {"id", id}, {"method", method}};
    if (!params.is_null()) request["params"] = params;
    return dumpLine(request);
}

json linkResult(const std::string& line, std::int64_t id) {
    const json answer = json::parse(line, nullptr, false);
    if (!answer.is_object()) throw std::runtime_error("the host's answer is not JSON");
    if (answer.contains("error")) {
        const json& e = answer["error"];
        throw std::runtime_error(e.is_object() ? e.value("message", std::string("the host refused the request")) : e.dump());
    }
    if (!answer.contains("id") || answer["id"] != json(id)) throw std::runtime_error("the host answered another request");
    if (!answer.contains("result")) throw std::runtime_error("the host's answer has no result");
    return answer["result"];
}

}  // namespace groomlab::agent
