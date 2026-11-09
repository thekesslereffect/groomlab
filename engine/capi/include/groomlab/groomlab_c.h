/* SPDX-License-Identifier: Apache-2.0 */
/*
 * GroomLab C API: a stable, ABI-safe interface to the GroomLab core for host bridges (Houdini,
 * Blender, game engines, pipeline tools). Everything here is plain C (C99): opaque handles, fixed
 * size integer types, no C++ types and no exceptions across the boundary. groomlab.hpp wraps it
 * for C++17 (RAII, exceptions).
 *
 * Versioning
 *   GROOMLAB_C_API_VERSION_MAJOR / _MINOR name the API this header declares; gl_api_version()
 *   gives the library's. The same major is binary compatible: a minor release only adds functions
 *   (and fields at the end of option structs, which carry their own size). A host checks the
 *   major at load time (GROOMLAB_C_API_COMPATIBLE).
 *
 * Errors
 *   Every function that can fail returns a gl_result: GL_OK (0) or an error code. After an error,
 *   gl_last_error() gives the message on the calling thread (thread-local, "" after a call that
 *   succeeded). On failure every out-parameter is set to NULL / 0, and a failing call leaves the
 *   session as it was (a bad graph, file or param keeps the previous one).
 *
 * Ownership
 *   - Input buffers are the caller's: the library copies what it keeps before returning.
 *   - Text and byte buffers handed out through `char**` out-parameters are allocated by the
 *     library and belong to the caller, who releases them with gl_free() (never free()).
 *     Text is UTF-8 and NUL-terminated.
 *   - A gl_strands is an evaluation's result, independent of the session: it stays valid (and
 *     unchanged) after further evaluations, edits or the session's destruction, until
 *     gl_strands_release(). Pointers its accessors return are views into it, valid until then.
 *   - Paths are UTF-8.
 *
 * Threads
 *   A session is used from one thread at a time (it evaluates in parallel inside); separate
 *   sessions may be used from separate threads. A gl_strands is read-only and may be read from any
 *   thread.
 *
 * Units and axes
 *   Points are in the groom's scene units (a .groom file's "units", cm by default) and its up axis
 *   ("upAxis", Y by default). A Z-up host turns the groom into its own axis with
 *   gl_session_set_up_axis (the core turns the scalp, guides, colliders and the params that are
 *   world directions); scaling units is the host's (a bridge multiplies points on its side).
 *
 * Time
 *   Frames become seconds by the one rule every host keeps (gl_frame_to_seconds: frame / fps, an
 *   NTSC rate such as 23.976 or 29.97 exact), so a frame evaluates the same here as in Maya, the
 *   CLI and the Arnold procedural.
 */
#ifndef GROOMLAB_C_H
#define GROOMLAB_C_H

#include <stddef.h>
#include <stdint.h>

#define GROOMLAB_C_API_VERSION_MAJOR 1
#define GROOMLAB_C_API_VERSION_MINOR 4
/* (major << 16) | minor, as gl_api_version() returns it. */
#define GROOMLAB_C_API_VERSION ((GROOMLAB_C_API_VERSION_MAJOR << 16) | GROOMLAB_C_API_VERSION_MINOR)
/* Whether a library reporting `version` (gl_api_version()) serves this header. */
#define GROOMLAB_C_API_COMPATIBLE(version) \
    (((version) >> 16) == GROOMLAB_C_API_VERSION_MAJOR && ((version) & 0xFFFFu) >= GROOMLAB_C_API_VERSION_MINOR)

#if defined(GROOMLAB_C_STATIC)
#define GL_API
#elif defined(_WIN32)
#if defined(GROOMLAB_C_BUILD)
#define GL_API __declspec(dllexport)
#else
#define GL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define GL_API __attribute__((visibility("default")))
#else
#define GL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Results ------------------------------------------------------------------------------ */

typedef int32_t gl_result;

enum {
    GL_OK = 0,
    GL_ERROR_INVALID_ARGUMENT = 1, /* a null handle or pointer, a size that does not add up */
    GL_ERROR_NOT_FOUND = 2,        /* no such node, param, attribute or operator type */
    GL_ERROR_TYPE_MISMATCH = 3,    /* a param read or written as another kind than it is */
    GL_ERROR_IO = 4,               /* a file that cannot be read or written */
    GL_ERROR_PARSE = 5,            /* text that is not a groom file / JSON of the expected form */
    GL_ERROR_GRAPH = 6,            /* a graph that does not load (unknown operator, bad connection) */
    GL_ERROR_EVALUATION = 7,       /* an evaluation that gave no strands (gl_session_eval_errors_json) */
    GL_ERROR_STATE = 8,            /* a call the session cannot take now (scalp points with no scalp) */
    GL_ERROR_OUT_OF_MEMORY = 9,
    GL_ERROR_INTERNAL = 10,        /* anything else: a bug, reported with its message */
    GL_ERROR_LICENSE = 11          /* reserved: no longer returned (API 1.4: GroomLab is free, nothing is gated) */
};

/* (major << 16) | minor of the library. */
GL_API uint32_t gl_api_version(void);
/* The GroomLab release the library was built from, e.g. "0.1.0" (static storage). */
GL_API const char* gl_version_string(void);
/* The message of the last failing call on this thread; "" after a call that succeeded. Valid
 * until the next gl_* call on this thread. Never NULL. */
GL_API const char* gl_last_error(void);
/* A gl_result's name, e.g. "GL_ERROR_NOT_FOUND" (static storage). */
GL_API const char* gl_result_name(gl_result result);
/* Releases a buffer the library handed out (char** out-parameters). NULL is fine. */
GL_API void gl_free(void* buffer);
/* Frame `frame` (fractional for subframes) at `fps` frames per second, in seconds, by the rule
 * every host keeps: frame / fps, a rate within a hair of an NTSC one (n * 1000 / 1001: 23.976,
 * 29.97, 59.94) taken as that rate exactly; a rate of 0 or less as 24. Since API 1.2. */
GL_API double gl_frame_to_seconds(double frame, double fps);

/* ---- Operators ---------------------------------------------------------------------------- */

/* Every registered operator type name, as a JSON array of strings. */
GL_API gl_result gl_operator_types_json(char** out_json);
/* Every operator's spec (inputs, outputs, params with kind, default, limits, groups...) as a JSON
 * array, the form the core's editors read. */
GL_API gl_result gl_operator_specs_json(char** out_json);
/* One operator's spec (an object of the array above). GL_ERROR_NOT_FOUND for an unknown type. */
GL_API gl_result gl_operator_spec_json(const char* type, char** out_json);
/* The graph a new session evaluates (hair out of the box), as JSON. */
GL_API gl_result gl_default_graph_json(char** out_json);

/* ---- Sessions ----------------------------------------------------------------------------- */

typedef struct gl_session gl_session;
typedef struct gl_strands gl_strands;

/* A session with the default graph and no inputs (no scalp: evaluations give no strands). */
GL_API gl_result gl_session_create(gl_session** out_session);
/* NULL is fine. Strands it gave stay valid. */
GL_API void gl_session_destroy(gl_session* session);

/* Replaces the session's groom with a .groom file's (graph, guides, scalp, parts, flow,
 * correctives, controls, maps, frame rate); relative paths in it (texture maps) resolve against
 * the file's folder, then its search paths. Animated caches the file names (scalp and guide
 * Alembic caches, simulation bakes) are not opened: the host feeds the moving scalp itself
 * (gl_session_update_scalp_points). GL_ERROR_IO, GL_ERROR_PARSE, GL_ERROR_GRAPH. */
GL_API gl_result gl_session_load_file(gl_session* session, const char* path);
/* The same from a .groom file's text in memory (`size` bytes, no NUL needed). `base_dir` (may be
 * NULL) stands for the file's folder. */
GL_API gl_result gl_session_load_memory(gl_session* session, const void* data, size_t size, const char* base_dir);
/* Writes the session's groom as a .groom file (atomically: written beside, then moved over): the
 * graph, guides, parts, flow, correctives, controls and the scalp as last set (after
 * gl_session_update_scalp_points: those points), with what the loaded file carried besides. */
GL_API gl_result gl_session_save_file(gl_session* session, const char* path);
/* The same into memory: `*out_data` (NUL-terminated, release with gl_free), `*out_size` bytes
 * without the NUL. `out_size` may be NULL. */
GL_API gl_result gl_session_save_memory(gl_session* session, char** out_data, size_t* out_size);
/* Deprecated since API 1.4, kept for bridges built against 1.3: GroomLab is free for any use, so
 * a groom carries no license tag. `*out_license` is always "" (release with gl_free); a .groom
 * file's old "license" field is ignored on load and not written. Since API 1.3. */
GL_API gl_result gl_session_license(gl_session* session, char** out_license);
/* Deprecated since API 1.4, kept for bridges built against 1.3: every groom may be exported, so
 * this is always GL_OK for a session (GL_ERROR_INVALID_ARGUMENT for NULL). Since API 1.3. */
GL_API gl_result gl_session_check_export(gl_session* session);

/* The scalp, in world space at the evaluation time: `point_count` points (3 floats each),
 * `face_count` polygons of `face_counts[i]` corners (3 or more) whose point indices follow one
 * another in `face_indices` (`index_count` = the sum of face_counts). `uvs` (may be NULL): 2
 * floats per polygon corner, in face_indices' order (a face-varying UV set). Polygons are
 * triangulated as fans from their first corner (corners 0, i, i + 1); Catmull-Clark (the graph's
 * Scalp node subdivision) refines the polygons, not the triangles. Replaces the scalp and its
 * rest pose. A loaded groom keeps the fingerprint of the scalp it was made on (its file's scalp
 * "topology": its point count and triangles); while the scalp set has another (another
 * vertex order, point count or triangulation: a quad split across its other diagonal too), every
 * evaluation lists "the scalp's vertex order or triangles differ from the groom's: strands are
 * rooted again by position" in gl_session_eval_errors_json (the strands come all the same). To
 * match a groom exactly, feed the file's own triangles (3-corner faces in its "triangles" order). */
GL_API gl_result gl_session_set_scalp(gl_session* session, const float* points, size_t point_count,
                                      const int32_t* face_counts, size_t face_count, const int32_t* face_indices,
                                      size_t index_count, const float* uvs);
/* The scalp's points at another time (a deforming scalp, every frame), on the topology and UVs
 * set before: `point_count` must be the scalp's. `rest_points` (may be NULL; point_count points):
 * the rest pose (an undeformed scalp: strands keep their count and roots while it deforms; also
 * what follow-the-scalp groom files evaluate on); NULL keeps the rest pose as it was.
 * GL_ERROR_STATE without a scalp, GL_ERROR_INVALID_ARGUMENT for another point count. */
GL_API gl_result gl_session_update_scalp_points(gl_session* session, const float* points, size_t point_count,
                                                const float* rest_points);
/* The scalp as given (0, 0 without one). Either pointer may be NULL. */
GL_API gl_result gl_session_scalp_size(gl_session* session, size_t* out_point_count, size_t* out_triangle_count);

/* The groom's up axis, "Y" or "Z" (a loaded file's "upAxis"; Y for a new session). Release with
 * gl_free. Since API 1.2. */
GL_API gl_result gl_session_get_up_axis(gl_session* session, char** out_axis);
/* Turns the groom into another up axis, "Y" or "Z" (GL_ERROR_INVALID_ARGUMENT for another): the
 * scalp and its rest pose, the guides, the colliders, and the graph's params that are world
 * directions (a spec's "worldDirection": Gravity's, Wind's, Bend's, Simulate's directions, Mirror's
 * axis) a quarter turn about X (Z-up (x, y, z) is Y-up (x, z, -y)), as Maya's import and the app
 * turn a file. Call it after loading a file, before feeding scalp points of the host's own axis;
 * saved files keep it. Nothing when the groom is in that axis already. Since API 1.2. */
GL_API gl_result gl_session_set_up_axis(gl_session* session, const char* axis);

/* The evaluation time in seconds (0 by default). */
GL_API gl_result gl_session_set_time(gl_session* session, double seconds);
GL_API gl_result gl_session_get_time(gl_session* session, double* out_seconds);
/* The time as a frame at the session's frame rate: seconds = gl_frame_to_seconds(frame, fps). */
GL_API gl_result gl_session_set_frame(gl_session* session, double frame);
/* Frames per second (24 by default; a groom file's own when loaded). Must be above 0. */
GL_API gl_result gl_session_set_frames_per_second(gl_session* session, double fps);
GL_API gl_result gl_session_get_frames_per_second(gl_session* session, double* out_fps);
/* A Control node's value by name (a host's live attribute driving the graph). */
GL_API gl_result gl_session_set_control(gl_session* session, const char* name, float value);
/* Where relative file paths in the graph (texture maps) resolve, after a loaded file's own folder:
 * `count` UTF-8 directories (environment variables allowed); 0 clears them. Saved with the file. */
GL_API gl_result gl_session_set_search_paths(gl_session* session, const char* const* directories, size_t count);

/* ---- Evaluation --------------------------------------------------------------------------- */

typedef struct gl_eval_options {
    uint32_t struct_size; /* sizeof(gl_eval_options) as the caller knows it: gl_eval_options_init */
    float density;        /* the preview density, 0-1 (1: every strand) */
    double point_budget;  /* points the result may hold (strands thinned to fit); 0: no limit */
    const char* view_node; /* a Strands node to show instead of the graph's output; NULL or "": the output */
    int32_t game_export;   /* nonzero: for a game engine (the Output node's rootDepth left off); 0: a render's */
} gl_eval_options;

/* Fills `options` with the defaults (density 1, no budget, the output, a render's) and its struct_size. */
GL_API void gl_eval_options_init(gl_eval_options* options);

/* Evaluates the groom at the session's time. `options` may be NULL (the defaults). On GL_OK
 * `*out_strands` holds the result (release with gl_strands_release); it may hold no strands (no
 * scalp, a graph that makes none). A graph that fails gives GL_ERROR_EVALUATION with the reasons
 * in gl_last_error() (and gl_session_eval_errors_json). Only what changed since the last
 * evaluation runs again (the graph's cache). */
GL_API gl_result gl_session_evaluate(gl_session* session, const gl_eval_options* options, gl_strands** out_strands);
/* What the last evaluation said went wrong (a node that failed, input that did not parse), one
 * string per line, as a JSON array; [] when nothing did. A partial result is GL_OK with lines here. */
GL_API gl_result gl_session_eval_errors_json(gl_session* session, char** out_json);

/* ---- Results ------------------------------------------------------------------------------ */

/* Releases a result; NULL is fine. */
GL_API void gl_strands_release(gl_strands* strands);
/* Counts (0 for NULL). */
GL_API size_t gl_strands_strand_count(const gl_strands* strands);
GL_API size_t gl_strands_point_count(const gl_strands* strands);
/* Point positions: 3 floats (x, y, z) per point, strand after strand. NULL when there are none. */
GL_API const float* gl_strands_positions(const gl_strands* strands);
/* Width (a diameter, scene units) per point. */
GL_API const float* gl_strands_widths(const gl_strands* strands);
/* Each strand's stable ID (the same strand keeps it from frame to frame and at other densities). */
GL_API const uint64_t* gl_strands_ids(const gl_strands* strands);
/* Points per strand (strand_count values). */
GL_API const int32_t* gl_strands_point_counts(const gl_strands* strands);
/* Each strand's first point (strand_count + 1 values: the last is point_count). */
GL_API const uint64_t* gl_strands_offsets(const gl_strands* strands);

/* Named attributes, float data. Scope: one value tuple per strand or per point. A name may be
 * there twice, once per scope (the color nodes' colorR/G/B with bands: per strand and per point). */
enum { GL_SCOPE_STRAND = 0, GL_SCOPE_POINT = 1 };
/* The attributes this result has: every float channel the graph wrote (per strand or per point:
 * melanin, melaninRedness, colorR/G/B, roughness, clumpId, groupId, iridescence...), and these
 * the core derives:
 *   "width"      per point, 1 component (as gl_strands_widths)
 *   "rootUV"     per strand, 2 components: the scalp's UV at the root (with scalp UVs)
 *   "color"      per strand, 3 components: linear RGB (HairColor's RGB, else from melanin)
 *   "pointColor" per point, 3 components: linear RGB, when the color varies along strands
 * A channel of the same name and scope as a derived attribute wins. Names are in a fixed order
 * (per strand ones first); the returned name is valid while the result lives. NULL for an index
 * out of range. */
GL_API size_t gl_strands_attribute_count(const gl_strands* strands);
GL_API const char* gl_strands_attribute_name(const gl_strands* strands, size_t index);
/* An attribute's data (`*out_data`: components values per strand or point, valid while the
 * result lives; NULL when it has no elements), its scope and component count. Any out-pointer
 * but out_data may be NULL. A name there in both scopes gives the per strand one
 * (gl_strands_attribute_scoped picks). GL_ERROR_NOT_FOUND for a name it does not have. */
GL_API gl_result gl_strands_attribute(const gl_strands* strands, const char* name, const float** out_data,
                                      int32_t* out_scope, int32_t* out_components);
/* The attribute at `index` (as gl_strands_attribute_name lists them): its scope and component
 * count (either may be NULL). GL_ERROR_NOT_FOUND for an index out of range. Since API 1.2. */
GL_API gl_result gl_strands_attribute_info(const gl_strands* strands, size_t index, int32_t* out_scope,
                                           int32_t* out_components);
/* An attribute by name and scope (GL_SCOPE_STRAND or GL_SCOPE_POINT): its data and component
 * count, as gl_strands_attribute gives them (`out_components` may be NULL). GL_ERROR_NOT_FOUND
 * when it has no attribute of that name in that scope. Since API 1.2. */
GL_API gl_result gl_strands_attribute_scoped(const gl_strands* strands, const char* name, int32_t scope,
                                             const float** out_data, int32_t* out_components);

/* ---- Graph -------------------------------------------------------------------------------- */
/* Nodes are named by their name in the graph (top level). Params hold JSON values: a float or int
 * param a number, a bool true/false, a vec3 an array of 3 numbers, a string a string, a ramp an
 * array of [position, value] pairs; a mappable number may hold a map instead (an object). A param
 * a node does not set reads as its operator's default. Setting a param re-evaluates only the
 * nodes it affects. */

/* The graph as JSON (the default graph's text for a session that has none of its own). */
GL_API gl_result gl_session_get_graph_json(gl_session* session, char** out_json);
/* Replaces the graph. GL_ERROR_GRAPH for one that does not load (the session keeps its graph). */
GL_API gl_result gl_session_set_graph_json(gl_session* session, const char* graph_json);
/* How many nodes the graph has (top level). */
GL_API gl_result gl_session_node_count(gl_session* session, size_t* out_count);
/* The node at `index` (in the graph's order). GL_ERROR_NOT_FOUND past the end. */
GL_API gl_result gl_session_node_name(gl_session* session, size_t index, char** out_name);
/* A node's operator type, e.g. "Scatter". */
GL_API gl_result gl_session_node_type(gl_session* session, const char* node, char** out_type);
/* A node's ports and params as the editors see them (gl_operator_spec_json's form, a Compound's
 * own inputs and exposed params included). */
GL_API gl_result gl_session_node_spec_json(gl_session* session, const char* node, char** out_json);
/* Raises a node to its operator's newest behaviour version, as the editors' Upgrade does: a node
 * saved with an older one ("outdated" in gl_session_node_spec_json, its "nodeVersion" below the
 * spec's "version") then evaluates as new nodes do, and params the newer version added take their
 * defaults. The look may change. `*out_upgraded` (may be NULL): 1 when the node was raised, 0
 * when it had the newest version already (nothing changes). GL_ERROR_NOT_FOUND for no such node.
 * Since API 1.1. */
GL_API gl_result gl_session_upgrade_node(gl_session* session, const char* node, int32_t* out_upgraded);

/* Any param as JSON text. */
GL_API gl_result gl_session_get_param_json(gl_session* session, const char* node, const char* param, char** out_json);
GL_API gl_result gl_session_set_param_json(gl_session* session, const char* node, const char* param,
                                           const char* value_json);
/* Typed access. GL_ERROR_TYPE_MISMATCH when the param holds (or takes) another kind: a float
 * param takes any number, an int param an integer; a float read of an int param converts. */
GL_API gl_result gl_session_get_param_float(gl_session* session, const char* node, const char* param, double* out_value);
GL_API gl_result gl_session_set_param_float(gl_session* session, const char* node, const char* param, double value);
GL_API gl_result gl_session_get_param_int(gl_session* session, const char* node, const char* param, int64_t* out_value);
GL_API gl_result gl_session_set_param_int(gl_session* session, const char* node, const char* param, int64_t value);
/* Bools are int32_t: 0 false, anything else true. */
GL_API gl_result gl_session_get_param_bool(gl_session* session, const char* node, const char* param, int32_t* out_value);
GL_API gl_result gl_session_set_param_bool(gl_session* session, const char* node, const char* param, int32_t value);
/* `value` / `out_value`: 3 doubles. */
GL_API gl_result gl_session_get_param_vec3(gl_session* session, const char* node, const char* param, double* out_value);
GL_API gl_result gl_session_set_param_vec3(gl_session* session, const char* node, const char* param, const double* value);
/* UTF-8 text; `*out_value` is released with gl_free. */
GL_API gl_result gl_session_get_param_string(gl_session* session, const char* node, const char* param, char** out_value);
GL_API gl_result gl_session_set_param_string(gl_session* session, const char* node, const char* param, const char* value);

#ifdef __cplusplus
}
#endif

#endif /* GROOMLAB_C_H */
