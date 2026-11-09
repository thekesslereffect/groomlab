// SPDX-License-Identifier: Apache-2.0
// The C API (groomlab_c.h) over GroomSession: every entry point catches everything, turns it into
// a gl_result and a thread-local message, and hands out only plain C data.

#include "groomlab/groomlab_c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/export_view.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/paths.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/schema.h"
#include "groomlab/version.h"

using nlohmann::json;
namespace fs = std::filesystem;

static_assert(sizeof(groomlab::Vec3f) == 3 * sizeof(float), "positions are handed out as packed floats");
static_assert(sizeof(std::array<float, 2>) == 2 * sizeof(float) && sizeof(std::array<float, 3>) == 3 * sizeof(float),
              "UVs and colors are handed out as packed floats");
static_assert(std::is_same<groomlab::StrandId, std::uint64_t>::value, "strand IDs are handed out as uint64_t");

struct gl_session {
    std::unique_ptr<groomlab::GroomSession> session;
    groomlab::GroomFile file;  // what a loaded file carried besides its data (file.data unused)
    double time = 0.0;
    std::vector<std::string> evalErrors;  // the last evaluation's
};

struct gl_strands {
    struct Attribute {
        std::string name;
        int32_t scope = GL_SCOPE_STRAND;
        int32_t components = 1;
        const float* data = nullptr;
    };
    groomlab::StrandsPtr strands;
    groomlab::ExportView view;  // counts, root UVs, colors
    std::vector<std::uint64_t> offsets;
    std::vector<Attribute> attributes;
};

namespace {

using namespace groomlab;

thread_local std::string tLastError;

// An error with its code, thrown inside an entry point.
struct ApiError {
    gl_result code;
    std::string message;
};

[[noreturn]] void fail(gl_result code, std::string message) { throw ApiError{code, std::move(message)}; }

gl_result failed(gl_result code, const char* message) {
    try {
        tLastError = message;
    } catch (...) {
        tLastError.clear();
    }
    return code;
}

// Runs an entry point's body: GL_OK and no message, or the error's code and message.
template <class F>
gl_result guard(F&& body) noexcept {
    try {
        tLastError.clear();
        body();
        return GL_OK;
    } catch (const ApiError& e) {
        return failed(e.code, e.message.c_str());
    } catch (const GraphError& e) {
        return failed(GL_ERROR_GRAPH, e.what());
    } catch (const GroomFileError& e) {
        return failed(GL_ERROR_PARSE, e.what());
    } catch (const SchemaError& e) {
        return failed(GL_ERROR_PARSE, e.what());
    } catch (const json::exception& e) {
        return failed(GL_ERROR_PARSE, e.what());
    } catch (const std::invalid_argument& e) {
        return failed(GL_ERROR_INVALID_ARGUMENT, e.what());
    } catch (const std::bad_alloc&) {
        return failed(GL_ERROR_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception& e) {
        return failed(GL_ERROR_INTERNAL, e.what());
    } catch (...) {
        return failed(GL_ERROR_INTERNAL, "unknown exception");
    }
}

void need(const void* p, const char* what) {
    if (!p) fail(GL_ERROR_INVALID_ARGUMENT, std::string(what) + " is NULL");
}

// A copy of `text` the caller releases with gl_free.
char* copyOut(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (!out) throw std::bad_alloc();
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

void giveText(char** out, const std::string& text) { *out = copyOut(text); }

GroomSession& sessionOf(gl_session* s) {
    need(s, "session");
    return *s->session;
}

// The relative paths of a groom: its file's folder, then its search paths.
void applySearchPaths(gl_session& s) {
    std::vector<std::string> dirs;
    if (!s.file.directory.empty()) dirs.push_back(s.file.directory);
    dirs.insert(dirs.end(), s.file.searchPaths.begin(), s.file.searchPaths.end());
    s.session->setPathResolver(dirs.empty() ? PathResolver{} : searchPathResolver(std::move(dirs)));
}

void adoptFile(gl_session& s, GroomFile file) {
    auto session = std::make_unique<GroomSession>(file.data);  // throws for a bad graph or guides
    if (!(file.framesPerSecond > 0.0)) file.framesPerSecond = 24.0;
    session->setFramesPerSecond(file.framesPerSecond);
    file.data = GroomData{};  // the session holds it
    s.session = std::move(session);
    s.file = std::move(file);
    s.evalErrors.clear();
    applySearchPaths(s);
}

// ---- The graph as JSON ---------------------------------------------------------------------------

bool blankGraph(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    const auto last = text.find_last_not_of(" \t\r\n");
    return text[first] == '{' && text[last] == '}' && text.find_first_not_of(" \t\r\n", first + 1) == last;
}

// The graph the session evaluates, as text: the default graph's for an empty one.
std::string graphText(const GroomSession& session) {
    const std::string& text = session.data().graphJson;
    return blankGraph(text) ? defaultGraphJson() : text;
}

json parseGraph(const GroomSession& session) {
    json graph = json::parse(graphText(session), nullptr, false);
    if (graph.is_discarded() || !graph.is_object()) fail(GL_ERROR_GRAPH, "the session's graph is not a JSON object");
    return graph;
}

json* findNode(json& graph, const std::string& name) {
    auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return nullptr;
    for (json& n : *nodes) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

json& nodeOf(json& graph, const char* node) {
    need(node, "node");
    json* n = findNode(graph, node);
    if (!n) fail(GL_ERROR_NOT_FOUND, std::string("no node '") + node + "'");
    return *n;
}

// The node's param spec (its operator's, a Compound's exposed one's), or null when it has none.
json paramSpec(const json& node, const std::string& param) {
    json spec;
    try {
        spec = json::parse(nodeSpecJson(node.dump()));
    } catch (const std::exception&) {
        return nullptr;  // an unknown type: the graph's own error says so
    }
    for (const json& p : spec.value("params", json::array())) {
        if (p.is_object() && p.value("name", std::string()) == param) return p;
    }
    return nullptr;
}

// A param's value: as set on the node, else its spec's default.
json paramValue(gl_session* s, const char* node, const char* param) {
    need(param, "param");
    json graph = parseGraph(sessionOf(s));
    const json& n = nodeOf(graph, node);
    const auto params = n.find("params");
    if (params != n.end() && params->is_object()) {
        const auto it = params->find(param);
        if (it != params->end()) return *it;
    }
    const json spec = paramSpec(n, param);
    if (spec.is_null()) fail(GL_ERROR_NOT_FOUND, std::string("node '") + node + "' has no param '" + param + "'");
    return spec.value("default", json(nullptr));
}

// Sets a param (checked against its kind, as the editors check it) and hands the graph to the
// session; a graph that does not load leaves the session as it was.
void setParam(gl_session* s, const char* node, const char* param, const json& value) {
    need(param, "param");
    GroomSession& session = sessionOf(s);
    json graph = parseGraph(session);
    json& n = nodeOf(graph, node);
    const json spec = paramSpec(n, param);
    if (spec.is_null()) fail(GL_ERROR_NOT_FOUND, std::string("node '") + node + "' has no param '" + param + "'");
    const std::string kind = spec.value("kind", std::string());
    const bool mapped = spec.value("mappable", false) && value.is_object();
    const bool fits = (kind == "bool" && value.is_boolean()) || (kind == "int" && (value.is_number_integer() || mapped)) ||
                      (kind == "float" && (value.is_number() || mapped)) || (kind == "string" && value.is_string()) ||
                      (kind == "vec3" && value.is_array() && value.size() == 3) || kind == "ramp" || kind == "map";
    if (!fits) {
        fail(GL_ERROR_TYPE_MISMATCH, std::string(node) + "." + param + " takes a " + kind +
                                         (spec.value("mappable", false) ? " or a map" : "") + ", not " + value.dump());
    }
    json& params = n["params"];
    if (!params.is_object()) params = json::object();
    params[param] = value;
    session.setGraphJson(graph.dump());
}

[[noreturn]] void mismatch(const char* node, const char* param, const json& value, const char* wanted) {
    fail(GL_ERROR_TYPE_MISMATCH, std::string(node) + "." + param + " holds " + value.dump() + ", not a " + wanted);
}

// ---- Operator specs -------------------------------------------------------------------------------

const json& specs() {
    static const json all = json::parse(operatorSpecsJson());
    return all;
}

// ---- Results --------------------------------------------------------------------------------------

std::unique_ptr<gl_strands> makeStrands(StrandsPtr strands, const ScalpMesh* scalp) {
    auto out = std::make_unique<gl_strands>();
    out->strands = std::move(strands);
    const StrandSet& s = *out->strands;
    out->view = describeExport(s, scalp, kExportPointColors);
    out->offsets.assign(s.offsets().begin(), s.offsets().end());
    if (out->offsets.empty()) out->offsets.push_back(0);

    // The channels, then what the core derives (a channel of the same name and scope wins). A
    // name in both scopes is listed once in each: per strand first.
    std::map<std::pair<std::string, int32_t>, bool> taken;
    auto add = [&](const std::string& name, int32_t scope, int32_t components, const float* data) {
        if (taken[{name, scope}]) return;
        taken[{name, scope}] = true;
        out->attributes.push_back({name, scope, components, data});
    };
    const std::size_t strandCount = s.strandCount(), pointCount = s.pointCount();
    for (const std::string& name : s.strandChannelNames()) {
        const std::vector<float>* values = s.strandChannel(name);
        if (values && values->size() == strandCount) add(name, GL_SCOPE_STRAND, 1, values->data());
    }
    for (const std::string& name : s.pointChannelNames()) {
        const std::vector<float>* values = s.pointChannel(name);
        if (values && values->size() == pointCount) add(name, GL_SCOPE_POINT, 1, values->data());
    }
    if (s.widths().size() == pointCount) add("width", GL_SCOPE_POINT, 1, s.widths().data());
    const ExportView& v = out->view;
    if (v.rootUVs.size() == strandCount && strandCount) add("rootUV", GL_SCOPE_STRAND, 2, v.rootUVs.front().data());
    if (v.colors.size() == strandCount && strandCount) add("color", GL_SCOPE_STRAND, 3, v.colors.front().data());
    if (v.pointColors.size() == pointCount && pointCount) add("pointColor", GL_SCOPE_POINT, 3, v.pointColors.front().data());
    return out;
}

// The groom as a file now: what the loaded file carried, the session's inputs as they are.
GroomFile currentFile(const gl_session& s) {
    GroomFile file = s.file;
    file.data = s.session->data();
    file.data.scalpHistory = nullptr;
    file.framesPerSecond = s.session->framesPerSecond();
    return file;
}
std::vector<Vec3f> toPoints(const float* xyz, std::size_t count) {
    std::vector<Vec3f> points(count);
    for (std::size_t i = 0; i < count; ++i) points[i] = Vec3f(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
    return points;
}

}  // namespace

// ==== General ========================================================================================

extern "C" {

GL_API uint32_t gl_api_version(void) { return GROOMLAB_C_API_VERSION; }

GL_API const char* gl_version_string(void) { return kVersionString; }

GL_API const char* gl_last_error(void) { return tLastError.c_str(); }

GL_API const char* gl_result_name(gl_result result) {
    switch (result) {
        case GL_OK: return "GL_OK";
        case GL_ERROR_INVALID_ARGUMENT: return "GL_ERROR_INVALID_ARGUMENT";
        case GL_ERROR_NOT_FOUND: return "GL_ERROR_NOT_FOUND";
        case GL_ERROR_TYPE_MISMATCH: return "GL_ERROR_TYPE_MISMATCH";
        case GL_ERROR_IO: return "GL_ERROR_IO";
        case GL_ERROR_PARSE: return "GL_ERROR_PARSE";
        case GL_ERROR_GRAPH: return "GL_ERROR_GRAPH";
        case GL_ERROR_EVALUATION: return "GL_ERROR_EVALUATION";
        case GL_ERROR_STATE: return "GL_ERROR_STATE";
        case GL_ERROR_OUT_OF_MEMORY: return "GL_ERROR_OUT_OF_MEMORY";
        case GL_ERROR_INTERNAL: return "GL_ERROR_INTERNAL";
        case GL_ERROR_LICENSE: return "GL_ERROR_LICENSE";
        default: return "GL_ERROR_UNKNOWN";
    }
}

GL_API void gl_free(void* buffer) { std::free(buffer); }

GL_API double gl_frame_to_seconds(double frame, double fps) { return frameToSeconds(frame, fps); }

// ==== Operators ======================================================================================

GL_API gl_result gl_operator_types_json(char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(out_json, "out_json");
        giveText(out_json, json(OperatorRegistry::instance().typeNames()).dump());
    });
}

GL_API gl_result gl_operator_specs_json(char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(out_json, "out_json");
        giveText(out_json, specs().dump());
    });
}

GL_API gl_result gl_operator_spec_json(const char* type, char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(type, "type");
        need(out_json, "out_json");
        for (const json& spec : specs()) {
            if (spec.value("type", std::string()) == type) return giveText(out_json, spec.dump());
        }
        fail(GL_ERROR_NOT_FOUND, std::string("no operator type '") + type + "'");
    });
}

GL_API gl_result gl_default_graph_json(char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(out_json, "out_json");
        giveText(out_json, defaultGraphJson());
    });
}

// ==== Sessions =======================================================================================

GL_API gl_result gl_session_create(gl_session** out_session) {
    if (out_session) *out_session = nullptr;
    return guard([&] {
        need(out_session, "out_session");
        auto s = std::make_unique<gl_session>();
        s->session = std::make_unique<GroomSession>();
        *out_session = s.release();
    });
}

GL_API void gl_session_destroy(gl_session* session) {
    try {
        delete session;
    } catch (...) {
    }
}

GL_API gl_result gl_session_load_file(gl_session* session, const char* path) {
    return guard([&] {
        need(session, "session");
        need(path, "path");
        const fs::path p = fs::u8path(path);
        std::ifstream in(p, std::ios::binary);
        if (!in) fail(GL_ERROR_IO, std::string("cannot read '") + path + "'");
        std::stringstream text;
        text << in.rdbuf();
        if (in.bad()) fail(GL_ERROR_IO, std::string("cannot read '") + path + "'");
        GroomFile file;
        try {
            file = groomFileFromJson(text.str(), p.parent_path().u8string());
        } catch (const GroomFileError& e) {
            fail(GL_ERROR_PARSE, std::string(path) + ": " + e.what());
        }
        adoptFile(*session, std::move(file));
    });
}

GL_API gl_result gl_session_load_memory(gl_session* session, const void* data, size_t size, const char* base_dir) {
    return guard([&] {
        need(session, "session");
        if (size) need(data, "data");
        const std::string text(static_cast<const char*>(data), size);
        adoptFile(*session, groomFileFromJson(text, base_dir ? base_dir : ""));
    });
}


GL_API gl_result gl_session_save_file(gl_session* session, const char* path) {
    return guard([&] {
        need(session, "session");
        need(path, "path");
        const std::string text = groomFileToJson(currentFile(*session));
        const fs::path target = fs::u8path(path);
        const fs::path tmp = tempPathFor(target);
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) fail(GL_ERROR_IO, std::string("cannot write '") + path + "'");
            out << text;
            out.close();
            if (!out) {
                std::error_code ec;
                fs::remove(tmp, ec);
                fail(GL_ERROR_IO, std::string("cannot write '") + path + "'");
            }
        }
        try {
            replaceFile(tmp, target);
        } catch (const std::runtime_error& e) {
            fail(GL_ERROR_IO, e.what());
        }
    });
}

GL_API gl_result gl_session_save_memory(gl_session* session, char** out_data, size_t* out_size) {
    if (out_data) *out_data = nullptr;
    if (out_size) *out_size = 0;
    return guard([&] {
        need(session, "session");
        need(out_data, "out_data");
        const std::string text = groomFileToJson(currentFile(*session));
        *out_data = copyOut(text);
        if (out_size) *out_size = text.size();
    });
}

GL_API gl_result gl_session_set_scalp(gl_session* session, const float* points, size_t point_count,
                                      const int32_t* face_counts, size_t face_count, const int32_t* face_indices,
                                      size_t index_count, const float* uvs) {
    return guard([&] {
        GroomSession& s = sessionOf(session);
        if (point_count) need(points, "points");
        if (face_count) need(face_counts, "face_counts");
        if (index_count) need(face_indices, "face_indices");
        std::size_t corners = 0, triangleCount = 0;
        for (std::size_t f = 0; f < face_count; ++f) {
            if (face_counts[f] < 3) fail(GL_ERROR_INVALID_ARGUMENT, "face " + std::to_string(f) + " has fewer than 3 corners");
            corners += static_cast<std::size_t>(face_counts[f]);
            triangleCount += static_cast<std::size_t>(face_counts[f]) - 2;
        }
        if (corners != index_count) {
            fail(GL_ERROR_INVALID_ARGUMENT, "the face counts add up to " + std::to_string(corners) + " corners, not index_count " +
                                                std::to_string(index_count));
        }
        for (std::size_t i = 0; i < index_count; ++i) {
            if (face_indices[i] < 0 || static_cast<std::size_t>(face_indices[i]) >= point_count) {
                fail(GL_ERROR_INVALID_ARGUMENT, "face index " + std::to_string(face_indices[i]) + " is not one of the " +
                                                    std::to_string(point_count) + " points");
            }
        }
        std::vector<ScalpMesh::Triangle> triangles;
        std::vector<ScalpMesh::UV> cornerUVs;
        std::vector<std::int32_t> polygons;
        triangles.reserve(triangleCount);
        polygons.reserve(face_count);
        if (uvs) cornerUVs.reserve(3 * triangleCount);
        std::size_t start = 0;
        auto uvOf = [&](std::size_t corner) { return ScalpMesh::UV{uvs[2 * corner], uvs[2 * corner + 1]}; };
        for (std::size_t f = 0; f < face_count; ++f) {
            const std::size_t n = static_cast<std::size_t>(face_counts[f]);
            for (std::size_t i = 1; i + 1 < n; ++i) {
                triangles.push_back({face_indices[start], face_indices[start + i], face_indices[start + i + 1]});
                if (uvs) {
                    cornerUVs.push_back(uvOf(start));
                    cornerUVs.push_back(uvOf(start + i));
                    cornerUVs.push_back(uvOf(start + i + 1));
                }
            }
            polygons.push_back(static_cast<std::int32_t>(n - 2));
            start += n;
        }
        auto mesh = std::make_shared<ScalpMesh>(toPoints(points, point_count), std::move(triangles));
        if (uvs) mesh->setCornerUVs(std::move(cornerUVs));
        mesh->setPolygons(std::move(polygons));
        s.setScalp(std::move(mesh));
    });
}

GL_API gl_result gl_session_update_scalp_points(gl_session* session, const float* points, size_t point_count,
                                                const float* rest_points) {
    return guard([&] {
        GroomSession& s = sessionOf(session);
        if (point_count) need(points, "points");
        const MeshPtr& scalp = s.rawScalp();
        if (!scalp) fail(GL_ERROR_STATE, "the session has no scalp: set one first (gl_session_set_scalp)");
        if (point_count != scalp->pointCount()) {
            fail(GL_ERROR_INVALID_ARGUMENT, "the scalp has " + std::to_string(scalp->pointCount()) + " points, not " +
                                                std::to_string(point_count));
        }
        if (rest_points) {
            const std::vector<Vec3f> rest = toPoints(rest_points, point_count);
            s.updateScalpPoints(toPoints(points, point_count), &rest);
        } else {
            s.updateScalpPoints(toPoints(points, point_count));
        }
    });
}

GL_API gl_result gl_session_scalp_size(gl_session* session, size_t* out_point_count, size_t* out_triangle_count) {
    if (out_point_count) *out_point_count = 0;
    if (out_triangle_count) *out_triangle_count = 0;
    return guard([&] {
        const MeshPtr& scalp = sessionOf(session).rawScalp();
        if (!scalp) return;
        if (out_point_count) *out_point_count = scalp->pointCount();
        if (out_triangle_count) *out_triangle_count = scalp->triangleCount();
    });
}

GL_API gl_result gl_session_license(gl_session* session, char** out_license) {
    if (out_license) *out_license = nullptr;
    return guard([&] {
        need(session, "session");
        need(out_license, "out_license");
        giveText(out_license, std::string());  // deprecated (API 1.4): grooms carry no license tag
    });
}

GL_API gl_result gl_session_check_export(gl_session* session) {
    // Deprecated (API 1.4): every groom may be exported.
    return guard([&] { need(session, "session"); });
}

GL_API gl_result gl_session_get_up_axis(gl_session* session, char** out_axis) {
    if (out_axis) *out_axis = nullptr;
    return guard([&] {
        need(session, "session");
        need(out_axis, "out_axis");
        giveText(out_axis, session->file.upAxis.empty() ? std::string("Y") : session->file.upAxis);
    });
}

GL_API gl_result gl_session_set_up_axis(gl_session* session, const char* axis) {
    return guard([&] {
        need(session, "session");
        need(axis, "axis");
        const std::string to = axis;
        if (to != "Y" && to != "Z") fail(GL_ERROR_INVALID_ARGUMENT, "the up axis is \"Y\" or \"Z\", not \"" + to + "\"");
        if (to == (session->file.upAxis.empty() ? std::string("Y") : session->file.upAxis)) return;
        // The groom as it is now (graph, guides, scalp, colliders...), turned, in a session of its own.
        GroomFile file = currentFile(*session);
        file.data.time = session->time;
        convertUpAxis(file, to);
        adoptFile(*session, std::move(file));
    });
}

GL_API gl_result gl_session_set_time(gl_session* session, double seconds) {
    return guard([&] {
        need(session, "session");
        session->time = seconds;
    });
}

GL_API gl_result gl_session_get_time(gl_session* session, double* out_seconds) {
    if (out_seconds) *out_seconds = 0.0;
    return guard([&] {
        need(session, "session");
        need(out_seconds, "out_seconds");
        *out_seconds = session->time;
    });
}

GL_API gl_result gl_session_set_frame(gl_session* session, double frame) {
    return guard([&] {
        const double fps = sessionOf(session).framesPerSecond();
        session->time = frameToSeconds(frame, fps);
    });
}

GL_API gl_result gl_session_set_frames_per_second(gl_session* session, double fps) {
    return guard([&] {
        GroomSession& s = sessionOf(session);
        if (!(fps > 0.0)) fail(GL_ERROR_INVALID_ARGUMENT, "frames per second must be above 0");
        s.setFramesPerSecond(fps);
        session->file.framesPerSecond = fps;
    });
}

GL_API gl_result gl_session_get_frames_per_second(gl_session* session, double* out_fps) {
    if (out_fps) *out_fps = 0.0;
    return guard([&] {
        need(out_fps, "out_fps");
        *out_fps = sessionOf(session).framesPerSecond();
    });
}

GL_API gl_result gl_session_set_control(gl_session* session, const char* name, float value) {
    return guard([&] {
        GroomSession& s = sessionOf(session);
        need(name, "name");
        std::map<std::string, float> controls = s.data().controls;
        controls[name] = value;
        s.setControls(std::move(controls));
    });
}

GL_API gl_result gl_session_set_search_paths(gl_session* session, const char* const* directories, size_t count) {
    return guard([&] {
        need(session, "session");
        if (count) need(directories, "directories");
        std::vector<std::string> dirs;
        for (std::size_t i = 0; i < count; ++i) {
            need(directories[i], "a directory");
            dirs.emplace_back(directories[i]);
        }
        session->file.searchPaths = std::move(dirs);
        applySearchPaths(*session);
    });
}

// ==== Evaluation =====================================================================================

GL_API void gl_eval_options_init(gl_eval_options* options) {
    if (!options) return;
    options->struct_size = static_cast<uint32_t>(sizeof(gl_eval_options));
    options->density = 1.0f;
    options->point_budget = 0.0;
    options->view_node = nullptr;
    options->game_export = 0;
}

GL_API gl_result gl_session_evaluate(gl_session* session, const gl_eval_options* options, gl_strands** out_strands) {
    if (out_strands) *out_strands = nullptr;
    return guard([&] {
        GroomSession& s = sessionOf(session);
        need(out_strands, "out_strands");
        // The caller's struct as far as it goes (an older caller's is shorter), the defaults past it.
        gl_eval_options o;
        gl_eval_options_init(&o);
        if (options) {
            if (options->struct_size < offsetof(gl_eval_options, density) + sizeof(float)) {
                fail(GL_ERROR_INVALID_ARGUMENT, "options->struct_size is not set (gl_eval_options_init)");
            }
            std::memcpy(&o, options, std::min<std::size_t>(options->struct_size, sizeof o));
        }
        if (!(o.density >= 0.0f && o.density <= 1.0f)) fail(GL_ERROR_INVALID_ARGUMENT, "density must be between 0 and 1");
        if (!(o.point_budget >= 0.0)) fail(GL_ERROR_INVALID_ARGUMENT, "point_budget must be 0 or more");

        ViewRequest request;
        request.density = o.density;
        request.pointBudget = o.point_budget;
        request.viewNode = o.view_node ? o.view_node : "";
        request.gameExport = o.game_export != 0;
        request.time = session->time;
        request.wantBounds = false;
        const ViewResult& view = s.evaluate(request);
        session->evalErrors = view.errors;
        if (!view.strands && !view.errors.empty()) {
            std::string message;
            for (const std::string& line : view.errors) message += (message.empty() ? "" : "\n") + line;
            fail(GL_ERROR_EVALUATION, message);
        }
        const StrandsPtr strands = view.strands ? view.strands : std::make_shared<const StrandSet>();
        *out_strands = makeStrands(strands, view.scalp.get()).release();
    });
}

GL_API gl_result gl_session_eval_errors_json(gl_session* session, char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(session, "session");
        need(out_json, "out_json");
        giveText(out_json, json(session->evalErrors).dump());
    });
}

// ==== Results ========================================================================================

GL_API void gl_strands_release(gl_strands* strands) {
    try {
        delete strands;
    } catch (...) {
    }
}

GL_API size_t gl_strands_strand_count(const gl_strands* strands) { return strands ? strands->strands->strandCount() : 0; }

GL_API size_t gl_strands_point_count(const gl_strands* strands) { return strands ? strands->strands->pointCount() : 0; }

GL_API const float* gl_strands_positions(const gl_strands* strands) {
    if (!strands || strands->strands->positions().empty()) return nullptr;
    return &strands->strands->positions().front().x;
}

GL_API const float* gl_strands_widths(const gl_strands* strands) {
    if (!strands || strands->strands->widths().empty()) return nullptr;
    return strands->strands->widths().data();
}

GL_API const uint64_t* gl_strands_ids(const gl_strands* strands) {
    if (!strands || strands->strands->ids().empty()) return nullptr;
    return strands->strands->ids().data();
}

GL_API const int32_t* gl_strands_point_counts(const gl_strands* strands) {
    if (!strands || strands->view.counts.empty()) return nullptr;
    return strands->view.counts.data();
}

GL_API const uint64_t* gl_strands_offsets(const gl_strands* strands) { return strands ? strands->offsets.data() : nullptr; }

GL_API size_t gl_strands_attribute_count(const gl_strands* strands) { return strands ? strands->attributes.size() : 0; }

GL_API const char* gl_strands_attribute_name(const gl_strands* strands, size_t index) {
    if (!strands || index >= strands->attributes.size()) return nullptr;
    return strands->attributes[index].name.c_str();
}

GL_API gl_result gl_strands_attribute(const gl_strands* strands, const char* name, const float** out_data,
                                      int32_t* out_scope, int32_t* out_components) {
    if (out_data) *out_data = nullptr;
    if (out_scope) *out_scope = 0;
    if (out_components) *out_components = 0;
    return guard([&] {
        need(strands, "strands");
        need(name, "name");
        need(out_data, "out_data");
        for (const gl_strands::Attribute& a : strands->attributes) {
            if (a.name != name) continue;
            *out_data = a.data;
            if (out_scope) *out_scope = a.scope;
            if (out_components) *out_components = a.components;
            return;
        }
        fail(GL_ERROR_NOT_FOUND, std::string("no attribute '") + name + "'");
    });
}

GL_API gl_result gl_strands_attribute_info(const gl_strands* strands, size_t index, int32_t* out_scope,
                                           int32_t* out_components) {
    if (out_scope) *out_scope = 0;
    if (out_components) *out_components = 0;
    return guard([&] {
        need(strands, "strands");
        if (index >= strands->attributes.size()) {
            fail(GL_ERROR_NOT_FOUND, "attribute index " + std::to_string(index) + " is past the " +
                                         std::to_string(strands->attributes.size()) + " attributes");
        }
        const gl_strands::Attribute& a = strands->attributes[index];
        if (out_scope) *out_scope = a.scope;
        if (out_components) *out_components = a.components;
    });
}

GL_API gl_result gl_strands_attribute_scoped(const gl_strands* strands, const char* name, int32_t scope,
                                             const float** out_data, int32_t* out_components) {
    if (out_data) *out_data = nullptr;
    if (out_components) *out_components = 0;
    return guard([&] {
        need(strands, "strands");
        need(name, "name");
        need(out_data, "out_data");
        if (scope != GL_SCOPE_STRAND && scope != GL_SCOPE_POINT) {
            fail(GL_ERROR_INVALID_ARGUMENT, "scope " + std::to_string(scope) + " is neither GL_SCOPE_STRAND nor GL_SCOPE_POINT");
        }
        for (const gl_strands::Attribute& a : strands->attributes) {
            if (a.name != name || a.scope != scope) continue;
            *out_data = a.data;
            if (out_components) *out_components = a.components;
            return;
        }
        fail(GL_ERROR_NOT_FOUND, std::string("no ") + (scope == GL_SCOPE_POINT ? "per point" : "per strand") +
                                     " attribute '" + name + "'");
    });
}

// ==== Graph ==========================================================================================

GL_API gl_result gl_session_get_graph_json(gl_session* session, char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        const GroomSession& s = sessionOf(session);
        need(out_json, "out_json");
        giveText(out_json, graphText(s));
    });
}

GL_API gl_result gl_session_set_graph_json(gl_session* session, const char* graph_json) {
    return guard([&] {
        GroomSession& s = sessionOf(session);
        need(graph_json, "graph_json");
        s.setGraphJson(graph_json);
    });
}

GL_API gl_result gl_session_node_count(gl_session* session, size_t* out_count) {
    if (out_count) *out_count = 0;
    return guard([&] {
        const GroomSession& s = sessionOf(session);
        need(out_count, "out_count");
        *out_count = GraphEdit(graphText(s)).nodeNames().size();
    });
}

GL_API gl_result gl_session_node_name(gl_session* session, size_t index, char** out_name) {
    if (out_name) *out_name = nullptr;
    return guard([&] {
        const GroomSession& s = sessionOf(session);
        need(out_name, "out_name");
        const std::vector<std::string> names = GraphEdit(graphText(s)).nodeNames();
        if (index >= names.size()) {
            fail(GL_ERROR_NOT_FOUND, "node index " + std::to_string(index) + " is past the graph's " +
                                         std::to_string(names.size()) + " nodes");
        }
        giveText(out_name, names[index]);
    });
}

GL_API gl_result gl_session_node_type(gl_session* session, const char* node, char** out_type) {
    if (out_type) *out_type = nullptr;
    return guard([&] {
        json graph = parseGraph(sessionOf(session));
        need(out_type, "out_type");
        giveText(out_type, nodeOf(graph, node).value("type", std::string()));
    });
}

GL_API gl_result gl_session_node_spec_json(gl_session* session, const char* node, char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        json graph = parseGraph(sessionOf(session));
        need(out_json, "out_json");
        giveText(out_json, nodeSpecJson(nodeOf(graph, node).dump()));
    });
}

GL_API gl_result gl_session_upgrade_node(gl_session* session, const char* node, int32_t* out_upgraded) {
    if (out_upgraded) *out_upgraded = 0;
    return guard([&] {
        GroomSession& s = sessionOf(session);
        need(node, "node");
        GraphEdit edit(graphText(s));
        if (!edit.hasNode(node)) fail(GL_ERROR_NOT_FOUND, std::string("no node '") + node + "'");
        if (!edit.upgradeNode(node)) return;  // the newest already: the graph as it was
        s.setGraphJson(edit.toJson());
        if (out_upgraded) *out_upgraded = 1;
    });
}

GL_API gl_result gl_session_get_param_json(gl_session* session, const char* node, const char* param, char** out_json) {
    if (out_json) *out_json = nullptr;
    return guard([&] {
        need(out_json, "out_json");
        giveText(out_json, paramValue(session, node, param).dump());
    });
}

GL_API gl_result gl_session_set_param_json(gl_session* session, const char* node, const char* param,
                                           const char* value_json) {
    return guard([&] {
        need(value_json, "value_json");
        json value = json::parse(value_json, nullptr, false);
        if (value.is_discarded()) fail(GL_ERROR_PARSE, std::string("not a JSON value: ") + value_json);
        setParam(session, node, param, value);
    });
}

GL_API gl_result gl_session_get_param_float(gl_session* session, const char* node, const char* param, double* out_value) {
    if (out_value) *out_value = 0.0;
    return guard([&] {
        need(out_value, "out_value");
        const json v = paramValue(session, node, param);
        if (!v.is_number()) mismatch(node, param, v, "number");
        *out_value = v.get<double>();
    });
}

GL_API gl_result gl_session_set_param_float(gl_session* session, const char* node, const char* param, double value) {
    return guard([&] { setParam(session, node, param, json(value)); });
}

GL_API gl_result gl_session_get_param_int(gl_session* session, const char* node, const char* param, int64_t* out_value) {
    if (out_value) *out_value = 0;
    return guard([&] {
        need(out_value, "out_value");
        const json v = paramValue(session, node, param);
        if (v.is_number_integer()) {
            *out_value = v.get<std::int64_t>();
        } else if (v.is_number_float() && v.get<double>() == static_cast<double>(static_cast<std::int64_t>(v.get<double>()))) {
            *out_value = static_cast<std::int64_t>(v.get<double>());
        } else {
            mismatch(node, param, v, "integer");
        }
    });
}

GL_API gl_result gl_session_set_param_int(gl_session* session, const char* node, const char* param, int64_t value) {
    return guard([&] { setParam(session, node, param, json(value)); });
}

GL_API gl_result gl_session_get_param_bool(gl_session* session, const char* node, const char* param, int32_t* out_value) {
    if (out_value) *out_value = 0;
    return guard([&] {
        need(out_value, "out_value");
        const json v = paramValue(session, node, param);
        if (!v.is_boolean()) mismatch(node, param, v, "bool");
        *out_value = v.get<bool>() ? 1 : 0;
    });
}

GL_API gl_result gl_session_set_param_bool(gl_session* session, const char* node, const char* param, int32_t value) {
    return guard([&] { setParam(session, node, param, json(value != 0)); });
}

GL_API gl_result gl_session_get_param_vec3(gl_session* session, const char* node, const char* param, double* out_value) {
    if (out_value) out_value[0] = out_value[1] = out_value[2] = 0.0;
    return guard([&] {
        need(out_value, "out_value");
        const json v = paramValue(session, node, param);
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) {
            mismatch(node, param, v, "vec3");
        }
        for (int i = 0; i < 3; ++i) out_value[i] = v[static_cast<std::size_t>(i)].get<double>();
    });
}

GL_API gl_result gl_session_set_param_vec3(gl_session* session, const char* node, const char* param, const double* value) {
    return guard([&] {
        need(value, "value");
        setParam(session, node, param, json::array({value[0], value[1], value[2]}));
    });
}

GL_API gl_result gl_session_get_param_string(gl_session* session, const char* node, const char* param, char** out_value) {
    if (out_value) *out_value = nullptr;
    return guard([&] {
        need(out_value, "out_value");
        const json v = paramValue(session, node, param);
        if (!v.is_string()) mismatch(node, param, v, "string");
        giveText(out_value, v.get<std::string>());
    });
}

GL_API gl_result gl_session_set_param_string(gl_session* session, const char* node, const char* param, const char* value) {
    return guard([&] {
        need(value, "value");
        setParam(session, node, param, json(std::string(value)));
    });
}

}  // extern "C"
