// SPDX-License-Identifier: Apache-2.0
#include "mcp_server.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "groomlab/version.h"
#include "groomlab/editor/agent_link.h"
#include "groomlab/editor/agent_tools.h"
#include "link_client.h"
#include "mcp_groom.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif
#endif

namespace groomlab::cli {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// JSON-RPC 2.0's error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

// A message longer than this is refused (the biggest a tool takes is a script or a guide list).
constexpr std::size_t kMaxMessageBytes = 64u << 20;

const char* const kAcceptedVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

const char* kInstructions =
    "GroomLab grows hair, fur and feathers on a mesh with a node graph (a scalp feeds Scatter, "
    "guides shape the strands through Interpolate, look nodes such as Clump, Curl, Noise, Length, "
    "HairColor and Width style them). Start with new_groom on a sample mesh (list_sample_meshes) "
    "and a preset (list_presets), look at it with render_preview, then refine: get_graph, "
    "describe_node, set_params, add_node. Groom guides by hand with brush_stroke (paths between "
    "scalp_landmarks such as crown and nape), select_guides, guide_op and paint_map, and check the "
    "result with measure_guides and render_preview (guides, compare, turntable, looks). Reference "
    "photos: add_photo, mesh_landmarks, align_photo, guides_from_photos. Root tubes: root_tube, "
    "tube_levels, tube_shape. Expressions: check_expression, then set_params. Simulation: "
    "animate_scalp or import_animation, simulate, sim_cache. Games: export_cards, export_game. "
    "Read get_docs for how a "
    "feature works. Every change is one step of undo, redo and history. Save with save_groom; "
    "export curves with export. Paths are relative to the server's root folder.";

json errorResponse(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json resultResponse(const json& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

json initializeResult(ToolBackend& tools, const json& params) {
    // The client's revision when this server speaks it, else the newest this server speaks.
    std::string version = kMcpProtocolVersion;
    if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        const std::string asked = params["protocolVersion"].get<std::string>();
        for (const char* v : kAcceptedVersions) {
            if (asked == v) version = asked;
        }
    }
    return {{"protocolVersion", version},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", "groomlab"}, {"title", "GroomLab"}, {"version", kVersionString}}},
            {"instructions", tools.about().empty() ? std::string(kInstructions) : tools.about() + " " + kInstructions}};
}

json handleOne(ToolBackend& tools, const json& message) {
    if (!message.is_object()) return errorResponse(nullptr, kInvalidRequest, "a message must be a JSON object");
    if (!message.contains("method") && (message.contains("result") || message.contains("error"))) return json();  // a response: none expected
    const bool isRequest = message.contains("id");
    const json id = isRequest ? message["id"] : json(nullptr);
    if (isRequest && !id.is_string() && !id.is_number_integer()) {
        return errorResponse(nullptr, kInvalidRequest, "the id must be a string or an integer");
    }
    if (message.value("jsonrpc", std::string()) != "2.0" || !message.contains("method") || !message["method"].is_string()) {
        return errorResponse(id, kInvalidRequest, "not a JSON-RPC 2.0 request");
    }
    const std::string method = message["method"].get<std::string>();
    const json params = message.value("params", json::object());
    if (!isRequest) return json();  // notifications (initialized, cancelled): nothing to answer

    try {
        if (method == "initialize") return resultResponse(id, initializeResult(tools, params));
        if (method == "ping") return resultResponse(id, json::object());
        if (method == "tools/list") return resultResponse(id, {{"tools", tools.list()}});
        if (method == "tools/call") {
            if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
                return errorResponse(id, kInvalidParams, "tools/call needs a tool \"name\"");
            }
            const std::string name = params["name"].get<std::string>();
            if (!tools.has(name)) return errorResponse(id, kInvalidParams, "unknown tool '" + name + "'");
            const json arguments = params.value("arguments", json::object());
            if (!arguments.is_object()) return errorResponse(id, kInvalidParams, "the arguments must be a JSON object");
            std::cerr << "groomlab mcp: " << name << "\n";
            const agent::ToolResult result = tools.call(name, arguments);
            return resultResponse(id, {{"content", result.content}, {"isError", result.isError}});
        }
        if (method == "resources/list") return resultResponse(id, {{"resources", json::array()}});
        if (method == "prompts/list") return resultResponse(id, {{"prompts", json::array()}});
        return errorResponse(id, kMethodNotFound, "unknown method '" + method + "'");
    } catch (const std::exception& e) {
        return errorResponse(id, kInternalError, e.what());
    }
}

// stdout becomes the protocol's alone; fd 1 (printf, std::cout) goes to stderr, so nothing the
// engine prints can break a message.
std::FILE* takeStdout() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    const int protocol = _dup(_fileno(stdout));
    if (protocol < 0) return nullptr;
    _setmode(protocol, _O_BINARY);
    std::fflush(stdout);
    if (_fileno(stderr) >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) == 0) {
        SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    } else {
        std::FILE* null = nullptr;
        freopen_s(&null, "NUL", "w", stdout);
    }
    return _fdopen(protocol, "wb");
#else
    signal(SIGPIPE, SIG_IGN);  // the client gone: writes fail, the loop ends at stdin's end
    const int protocol = dup(STDOUT_FILENO);
    if (protocol < 0) return nullptr;
    std::fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    return fdopen(protocol, "wb");
#endif
}

std::string executableDir(const char* argv0) {
#ifdef _WIN32
    (void)argv0;
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return fs::path(path).parent_path().u8string();
#elif defined(__APPLE__)
    // macOS has no /proc: dyld's path of the executable (through any symlink: made canonical).
    char path[4096];
    std::uint32_t size = sizeof path;
    std::error_code ec;
    if (_NSGetExecutablePath(path, &size) == 0) {
        const fs::path self = fs::weakly_canonical(fs::u8path(path), ec);
        if (!ec) return self.parent_path().u8string();
    }
    return fs::absolute(fs::u8path(argv0 ? argv0 : ""), ec).parent_path().u8string();
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path().u8string();
    return fs::absolute(fs::u8path(argv0 ? argv0 : ""), ec).parent_path().u8string();
#endif
}

void send(std::FILE* out, const json& message) {
    // Invalid UTF-8 (a path the OS gave in another code page) is replaced, never a broken line.
    const std::string line = message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);
}

}  // namespace

namespace {

// ---- Live: a host's tools over the link -----------------------------------------------------------

// The tools of a running host (the named one, or the newest), forwarded over its link; with none
// at the start, the server's own (headless), said in the instructions and the first answer. A host
// gone away: found again (a new link of the same host), else every call says so.
class LiveBackend : public ToolBackend {
public:
    LiveBackend(std::string host, agent::AgentTools& headless) : host_(std::move(host)), headless_(headless) {
        connect();
        if (!client_) std::cerr << "groomlab mcp --live: no " << (host_.empty() ? std::string("host") : host_) << " is running its agent link; headless\n";
    }

    nlohmann::json list() override {
        if (!live()) return headless_.list();
        return tools_;
    }
    bool has(const std::string& name) override {
        if (!live()) return headless_.has(name);
        for (const json& t : tools_) {
            if (t.value("name", std::string()) == name) return true;
        }
        return false;
    }
    agent::ToolResult call(const std::string& name, const json& arguments) override {
        // A host that went away: looked for again (its link turned on again) before each call.
        if (!client_ && lastHost_) {
            if (!connect(lastHost_->host)) return wentAway(*lastHost_);
        }
        if (!client_) {
            agent::ToolResult r = headless_.call(name, arguments);
            if (!toldHeadless_) {
                toldHeadless_ = true;
                r.content.insert(r.content.begin(), json{{"type", "text"}, {"text", "(No GroomLab host runs its agent link: this server works headless, on "
                                                                "grooms of its own. Turn on Agent link in Maya's Groom Tools (the ... "
                                                                "menu) and restart the server to work on the open groom.)"}});
            }
            return r;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                const json result = client_->request("tools/call", {{"name", name}, {"arguments", arguments}});
                agent::ToolResult r;
                r.content = result.value("content", json::array());
                r.isError = result.value("isError", false);
                return r;
            } catch (const LinkLost&) {
                const agent::LinkInfo was = client_->info();
                client_.reset();
                if (attempt == 0 && connect(was.host)) continue;  // (the host's link restarted)
                return wentAway(was);
            } catch (const std::exception& e) {
                agent::ToolResult r;
                r.isError = true;
                r.content.push_back({{"type", "text"}, {"text", json{{"error", std::string("the host refused: ") + e.what()}}.dump()}});
                return r;
            }
        }
        return {};
    }
    std::string about() override {
        if (!client_) {
            return "No GroomLab host is running its agent link now, so these tools work headless, on grooms this server opens "
                   "itself (turn on Agent link in Maya's Groom Tools and restart the server to work on the open groom).";
        }
        const agent::LinkInfo& info = client_->info();
        return "Live: these tools work on the groom open in " + info.host + " (pid " + std::to_string(info.pid) + ")" +
               (info.groom.empty() ? std::string() : ", " + info.groom) +
               ": every change is one step of the host's own undo, and the artist sees it at once.";
    }

private:
    bool live() const { return client_ || lastHost_; }

    static agent::ToolResult wentAway(const agent::LinkInfo& was) {
        agent::ToolResult r;
        r.isError = true;
        r.content.push_back({{"type", "text"}, {"text", json{{"error", was.host + " (pid " + std::to_string(was.pid) +
                                                                            ") went away: its agent link stopped or it quit. Turn the "
                                                                            "link on again; the next call finds it."}}.dump()}});
        return r;
    }

    // The named (or newest) host's link connected, and its tools read; false when none answers.
    bool connect(const std::string& host = {}) {
        const std::string wanted = host.empty() ? host_ : host;
        for (const agent::LinkInfo& info : agent::liveLinks(wanted)) {
            try {
                auto client = std::make_unique<LinkClient>(info);
                const json result = client->request("tools/list");
                tools_ = result.value("tools", json::array());
                client_ = std::move(client);
                lastHost_ = info;
                std::cerr << "groomlab mcp --live: " << info.host << " (pid " << info.pid << ") on port " << info.port << "\n";
                return true;
            } catch (const std::exception& e) {
                std::cerr << "groomlab mcp --live: " << info.host << " (pid " << info.pid << "): " << e.what() << "\n";
            }
        }
        return false;
    }

    std::string host_;
    agent::AgentTools& headless_;
    std::unique_ptr<LinkClient> client_;
    json tools_ = json::array();
    bool toldHeadless_ = false;
    std::optional<agent::LinkInfo> lastHost_;  // the host connected last (a live server stays live)
};

}  // namespace

json handleMcpMessage(ToolBackend& tools, const json& message) {
    if (message.is_array()) {  // a batch (2025-03-26): the responses together, none for notifications alone
        if (message.empty()) return errorResponse(nullptr, kInvalidRequest, "an empty batch");
        json responses = json::array();
        for (const json& m : message) {
            json r = handleOne(tools, m);
            if (!r.is_null()) responses.push_back(std::move(r));
        }
        return responses.empty() ? json() : responses;
    }
    return handleOne(tools, message);
}

int runMcpServer(int argc, char** argv) {
    std::string root, liveHost;
    bool live = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        } else if (arg == "--live") {
            live = true;
            if (i + 1 < argc && (std::string(argv[i + 1]) == "maya" || std::string(argv[i + 1]) == "blender" ||
                                 std::string(argv[i + 1]) == "app")) {
                liveHost = argv[++i];
            }
        } else {
            std::cerr << "usage: groomlab-cli mcp [--root <dir>] [--live [maya|blender|app]]\n"
                         "Speaks the Model Context Protocol on stdin/stdout. Files are read and written only under\n"
                         "the root (default: the current folder). --live: the tools work on the groom open in a\n"
                         "running host whose Agent link is on (the named one, else the newest); none: headless.\n";
            return 2;
        }
    }
    std::error_code ec;
    fs::path rootPath = root.empty() ? fs::current_path(ec) : fs::u8path(root);
    rootPath = fs::weakly_canonical(fs::absolute(rootPath, ec), ec);
    if (ec || !fs::is_directory(rootPath, ec)) {
        std::cerr << "groomlab-cli mcp: the root '" << root << "' is not a folder\n";
        return 2;
    }
    std::FILE* out = takeStdout();
    if (!out) {
        std::cerr << "groomlab-cli mcp: cannot take stdout for the protocol\n";
        return 1;
    }
    std::cerr << "groomlab mcp " << kVersionString << ": root " << rootPath.u8string() << "\n";
    HeadlessTarget target(rootPath.u8string(), executableDir(argc > 0 ? argv[0] : nullptr));
    agent::AgentTools agentTools(target);
    HeadlessBackend headless(agentTools);
    std::unique_ptr<LiveBackend> liveBackend;
    if (live) liveBackend = std::make_unique<LiveBackend>(liveHost, agentTools);
    ToolBackend& tools = liveBackend ? static_cast<ToolBackend&>(*liveBackend) : static_cast<ToolBackend&>(headless);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        json response;
        if (line.size() > kMaxMessageBytes) {
            response = errorResponse(nullptr, kInvalidRequest, "the message is too long");
        } else {
            json message = json::parse(line, nullptr, false);
            response = message.is_discarded() ? errorResponse(nullptr, kParseError, "not JSON") : handleMcpMessage(tools, message);
        }
        if (!response.is_null()) send(out, response);
    }
    return 0;
}

}  // namespace groomlab::cli
