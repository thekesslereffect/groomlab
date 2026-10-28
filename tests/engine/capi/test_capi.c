/* SPDX-License-Identifier: Apache-2.0 */
/* The C API (groomlab_c.h) from C: compiled as C, linked to the groomlab shared library only.
 * Loads a groom file, evaluates it, reads the results and attributes, round-trips the file through
 * memory and params through the graph, builds a scalp from polygons, upgrades nodes, and checks the
 * error paths. Built twice: against the shared library, and with the C API linked in statically
 * beside a test operator at version 2 (GROOMLAB_CAPI_TEST_LIFT), for a node upgrade that does
 * something. */

#include "groomlab/groomlab_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                         \
    do {                                                                                                    \
        if (!(cond)) {                                                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s (last error: %s)\n", __FILE__, __LINE__, #cond, gl_last_error()); \
            ++failures;                                                                                     \
        }                                                                                                   \
    } while (0)

#define CHECK_RESULT(call, expected)                                                                         \
    do {                                                                                                     \
        gl_result r_ = (call);                                                                               \
        if (r_ != (expected)) {                                                                              \
            fprintf(stderr, "%s:%d: %s gave %s, expected %s (%s)\n", __FILE__, __LINE__, #call, gl_result_name(r_), \
                    gl_result_name(expected), gl_last_error());                                              \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)

#define CHECK_OK(call) CHECK_RESULT(call, GL_OK)

static const char* compatGroom(void) { return GROOMLAB_CAPI_COMPAT_GROOM; }

/* Counts, offsets and point counts agree; every position is a finite number. */
static void checkConsistent(const gl_strands* s) {
    size_t strands = gl_strands_strand_count(s), points = gl_strands_point_count(s), i, total = 0;
    const int32_t* counts = gl_strands_point_counts(s);
    const uint64_t* offsets = gl_strands_offsets(s);
    const float* p = gl_strands_positions(s);
    CHECK(offsets != NULL);
    if (!strands) return;
    CHECK(counts != NULL && p != NULL && gl_strands_ids(s) != NULL && gl_strands_widths(s) != NULL);
    if (!counts || !offsets || !p) return;
    for (i = 0; i < strands; ++i) {
        CHECK(offsets[i] == total);
        total += (size_t)counts[i];
    }
    CHECK(total == points);
    CHECK(offsets[strands] == points);
    for (i = 0; i < 3 * points; ++i) {
        if (!isfinite(p[i])) {
            CHECK(!"a position is not finite");
            break;
        }
    }
}

static int hasText(const char* text, const char* part) { return text && strstr(text, part) != NULL; }

static void testVersion(void) {
    CHECK(GROOMLAB_C_API_COMPATIBLE(gl_api_version()));
    CHECK(strlen(gl_version_string()) > 0);
    CHECK(strcmp(gl_result_name(GL_ERROR_NOT_FOUND), "GL_ERROR_NOT_FOUND") == 0);
}

static void testOperators(void) {
    char* json = NULL;
    CHECK_OK(gl_operator_types_json(&json));
    CHECK(hasText(json, "\"Scatter\""));
    gl_free(json);
    CHECK_OK(gl_operator_specs_json(&json));
    CHECK(hasText(json, "\"params\""));
    gl_free(json);
    CHECK_OK(gl_operator_spec_json("Scatter", &json));
    CHECK(hasText(json, "\"density\""));
    gl_free(json);
    CHECK_RESULT(gl_operator_spec_json("NoSuchOperator", &json), GL_ERROR_NOT_FOUND);
    CHECK(json == NULL);
    CHECK(hasText(gl_last_error(), "NoSuchOperator"));
    CHECK_OK(gl_default_graph_json(&json));
    CHECK(hasText(json, "\"Scatter\""));
    gl_free(json);
}

static void testLoadEvaluateSave(void) {
    gl_session* session = NULL;
    gl_session* again = NULL;
    gl_strands* strands = NULL;
    gl_strands* reloaded = NULL;
    char* data = NULL;
    size_t size = 0, i, n, points = 0, triangles = 0;
    double fps = 0.0;

    CHECK_OK(gl_session_create(&session));
    CHECK_OK(gl_session_load_file(session, compatGroom()));
    CHECK(strcmp(gl_last_error(), "") == 0);
    CHECK_OK(gl_session_scalp_size(session, &points, &triangles));
    CHECK(points == 266 && triangles == 528);
    CHECK_OK(gl_session_get_frames_per_second(session, &fps));
    CHECK(fps > 0.0);
    CHECK_OK(gl_session_evaluate(session, NULL, &strands));
    CHECK(strands != NULL);
    CHECK(gl_strands_strand_count(strands) > 100);
    checkConsistent(strands);

    /* Attributes: every listed name reads back, by name and in its scope; the derived ones are there. */
    n = gl_strands_attribute_count(strands);
    CHECK(n > 0);
    for (i = 0; i < n; ++i) {
        const char* name = gl_strands_attribute_name(strands, i);
        const float* values = NULL;
        int32_t scope = -1, components = 0, listedScope = -1, listedComponents = 0;
        CHECK(name != NULL);
        CHECK_OK(gl_strands_attribute(strands, name, &values, &scope, &components));
        CHECK(values != NULL && (scope == GL_SCOPE_STRAND || scope == GL_SCOPE_POINT) && components >= 1);
        CHECK_OK(gl_strands_attribute_info(strands, i, &listedScope, &listedComponents));
        CHECK_OK(gl_strands_attribute_scoped(strands, name, listedScope, &values, &components));
        CHECK(values != NULL && components == listedComponents);
    }
    CHECK(gl_strands_attribute_name(strands, n) == NULL);
    {
        int32_t scope = -1;
        const float* values = NULL;
        CHECK_RESULT(gl_strands_attribute_info(strands, n, &scope, NULL), GL_ERROR_NOT_FOUND);
        CHECK_RESULT(gl_strands_attribute_scoped(strands, "rootUV", GL_SCOPE_POINT, &values, NULL), GL_ERROR_NOT_FOUND);
        CHECK(values == NULL);
        CHECK_RESULT(gl_strands_attribute_scoped(strands, "rootUV", 7, &values, NULL), GL_ERROR_INVALID_ARGUMENT);
    }
    {
        const float* values = NULL;
        int32_t scope = -1, components = 0;
        CHECK_OK(gl_strands_attribute(strands, "width", &values, &scope, &components));
        CHECK(values == gl_strands_widths(strands) && scope == GL_SCOPE_POINT && components == 1);
        CHECK_OK(gl_strands_attribute(strands, "rootUV", &values, &scope, &components));
        CHECK(scope == GL_SCOPE_STRAND && components == 2);
        CHECK_OK(gl_strands_attribute(strands, "clumpId", &values, &scope, &components));
        CHECK(scope == GL_SCOPE_STRAND && components == 1);
        CHECK_RESULT(gl_strands_attribute(strands, "noSuchAttribute", &values, &scope, &components), GL_ERROR_NOT_FOUND);
        CHECK(values == NULL);
    }

    /* Through memory and back: the same strands, bit for bit. */
    CHECK_OK(gl_session_save_memory(session, &data, &size));
    CHECK(data != NULL && size > 0 && strlen(data) == size);
    CHECK_OK(gl_session_create(&again));
    CHECK_OK(gl_session_load_memory(again, data, size, NULL));
    gl_free(data);
    /* The file's texture map is relative to its folder: the session reading from memory has none,
     * so it resolves through a search path. */
    {
        char dir[1024];
        const char* path = compatGroom();
        const char* slash = strrchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : 0;
        const char* dirs[1];
        if (len >= sizeof dir) len = sizeof dir - 1;
        memcpy(dir, path, len);
        dir[len] = '\0';
        dirs[0] = dir;
        CHECK_OK(gl_session_set_search_paths(again, dirs, 1));
    }
    CHECK_OK(gl_session_evaluate(again, NULL, &reloaded));
    CHECK(gl_strands_strand_count(reloaded) == gl_strands_strand_count(strands));
    CHECK(gl_strands_point_count(reloaded) == gl_strands_point_count(strands));
    if (gl_strands_point_count(reloaded) == gl_strands_point_count(strands) && gl_strands_point_count(strands)) {
        CHECK(memcmp(gl_strands_positions(reloaded), gl_strands_positions(strands),
                     3 * sizeof(float) * gl_strands_point_count(strands)) == 0);
        CHECK(memcmp(gl_strands_ids(reloaded), gl_strands_ids(strands), sizeof(uint64_t) * gl_strands_strand_count(strands)) == 0);
    }
    gl_strands_release(reloaded);
    gl_session_destroy(again);

    /* A result outlives its session. */
    gl_session_destroy(session);
    checkConsistent(strands);
    gl_strands_release(strands);
}

static void testParams(void) {
    gl_session* session = NULL;
    gl_strands* before = NULL;
    gl_strands* after = NULL;
    double f = 0.0, v[3] = {0.0, 0.0, 0.0};
    const double up[3] = {0.0, 1.0, 0.0};
    int64_t count = 0;
    int32_t flag = 0;
    char* text = NULL;
    size_t nodes = 0;

    CHECK_OK(gl_session_create(&session));
    CHECK_OK(gl_session_load_file(session, compatGroom()));
    CHECK_OK(gl_session_evaluate(session, NULL, &before));

    CHECK_OK(gl_session_node_count(session, &nodes));
    CHECK(nodes == 20);
    CHECK_OK(gl_session_node_name(session, 0, &text));
    CHECK(text && strcmp(text, "scalp") == 0);
    gl_free(text);
    CHECK_RESULT(gl_session_node_name(session, nodes, &text), GL_ERROR_NOT_FOUND);
    CHECK_OK(gl_session_node_type(session, "clump2", &text));
    CHECK(text && strcmp(text, "Clump") == 0);
    gl_free(text);
    CHECK_OK(gl_session_node_spec_json(session, "noise", &text));
    CHECK(hasText(text, "\"amplitude\""));
    gl_free(text);

    /* Typed reads of what the file holds, and of a default. */
    CHECK_OK(gl_session_get_param_float(session, "noise", "amplitude", &f));
    CHECK(f == 0.03);
    CHECK_OK(gl_session_get_param_int(session, "clump", "count", &count));
    CHECK(count == 25);
    CHECK_OK(gl_session_get_param_vec3(session, "comb", "direction", v));
    CHECK(v[0] == 0.0 && v[1] == -1.0 && v[2] == 0.2);
    CHECK_OK(gl_session_get_param_string(session, "attr", "name", &text));
    CHECK(text && strcmp(text, "len") == 0);
    gl_free(text);
    CHECK_OK(gl_session_get_param_float(session, "clump2", "strength", &f));
    CHECK(f == 0.3);
    CHECK_OK(gl_session_get_param_json(session, "scatter", "density", &text));
    CHECK(hasText(text, "density.pgm"));
    gl_free(text);

    /* Writes, read back, and the evaluation follows. */
    CHECK_OK(gl_session_set_param_float(session, "noise", "amplitude", 0.25));
    CHECK_OK(gl_session_get_param_float(session, "noise", "amplitude", &f));
    CHECK(f == 0.25);
    CHECK_OK(gl_session_set_param_int(session, "clump", "count", 40));
    CHECK_OK(gl_session_get_param_int(session, "clump", "count", &count));
    CHECK(count == 40);
    CHECK_OK(gl_session_set_param_vec3(session, "comb", "direction", up));
    CHECK_OK(gl_session_get_param_vec3(session, "comb", "direction", v));
    CHECK(v[0] == 0.0 && v[1] == 1.0 && v[2] == 0.0);
    CHECK_OK(gl_session_set_param_json(session, "clump2", "strength", "0.5"));
    CHECK_OK(gl_session_get_param_float(session, "clump2", "strength", &f));
    CHECK(f == 0.5);
    CHECK_OK(gl_session_evaluate(session, NULL, &after));
    CHECK(gl_strands_strand_count(after) == gl_strands_strand_count(before));
    if (gl_strands_point_count(after) == gl_strands_point_count(before) && gl_strands_point_count(before)) {
        CHECK(memcmp(gl_strands_positions(after), gl_strands_positions(before), 3 * sizeof(float) * gl_strands_point_count(before)) != 0);
    }
    gl_strands_release(after);

    /* Kind mismatches, unknown names, bad JSON: errors, and the graph as it was. */
    CHECK_RESULT(gl_session_get_param_float(session, "scatter", "density", &f), GL_ERROR_TYPE_MISMATCH);  /* mapped */
    CHECK_RESULT(gl_session_set_param_string(session, "noise", "amplitude", "loud"), GL_ERROR_TYPE_MISMATCH);
    CHECK(hasText(gl_last_error(), "amplitude"));
    CHECK_RESULT(gl_session_set_param_bool(session, "noise", "amplitude", 1), GL_ERROR_TYPE_MISMATCH);
    CHECK_RESULT(gl_session_set_param_float(session, "clump", "count", 2.5), GL_ERROR_TYPE_MISMATCH);
    CHECK_RESULT(gl_session_get_param_bool(session, "noise", "amplitude", &flag), GL_ERROR_TYPE_MISMATCH);
    CHECK_RESULT(gl_session_get_param_float(session, "nobody", "amplitude", &f), GL_ERROR_NOT_FOUND);
    CHECK(hasText(gl_last_error(), "nobody"));
    CHECK_RESULT(gl_session_set_param_float(session, "noise", "loudness", 1.0), GL_ERROR_NOT_FOUND);
    CHECK_RESULT(gl_session_set_param_json(session, "noise", "amplitude", "{not json"), GL_ERROR_PARSE);
    CHECK_OK(gl_session_get_param_float(session, "noise", "amplitude", &f));
    CHECK(f == 0.25);
    CHECK_RESULT(gl_session_set_graph_json(session, "{\"nodes\": [{\"name\": \"x\", \"type\": \"NoSuchOperator\"}]}"), GL_ERROR_GRAPH);
    CHECK(strlen(gl_last_error()) > 0);
    CHECK_OK(gl_session_node_count(session, &nodes));
    CHECK(nodes == 20);

    gl_strands_release(before);
    gl_session_destroy(session);
}

/* A grid of quads in the XZ plane, facing +Y, with UVs per corner. */
#define GRID 10
static void testScalpFromPolygons(void) {
    float points[(GRID + 1) * (GRID + 1) * 3], moved[(GRID + 1) * (GRID + 1) * 3];
    int32_t counts[GRID * GRID], indices[GRID * GRID * 4];
    float uvs[GRID * GRID * 4 * 2];
    gl_session* session = NULL;
    gl_strands* strands = NULL;
    gl_strands* lifted = NULL;
    gl_strands* thinned = NULL;
    gl_eval_options options;
    size_t points_n = 0, triangles = 0, i;
    int x, z, k = 0, c = 0;

    for (z = 0; z <= GRID; ++z) {
        for (x = 0; x <= GRID; ++x) {
            float* p = points + 3 * (z * (GRID + 1) + x);
            p[0] = 2.0f * (float)x - GRID;
            p[1] = 0.0f;
            p[2] = 2.0f * (float)z - GRID;
        }
    }
    for (z = 0; z < GRID; ++z) {
        for (x = 0; x < GRID; ++x) {
            int corner[4][2], j;
            corner[0][0] = x, corner[0][1] = z;
            corner[1][0] = x, corner[1][1] = z + 1;
            corner[2][0] = x + 1, corner[2][1] = z + 1;
            corner[3][0] = x + 1, corner[3][1] = z;
            counts[c++] = 4;
            for (j = 0; j < 4; ++j) {
                indices[k] = corner[j][1] * (GRID + 1) + corner[j][0];
                uvs[2 * k] = (float)corner[j][0] / GRID;
                uvs[2 * k + 1] = (float)corner[j][1] / GRID;
                ++k;
            }
        }
    }

    CHECK_OK(gl_session_create(&session));
    CHECK_RESULT(gl_session_update_scalp_points(session, points, (GRID + 1) * (GRID + 1), NULL), GL_ERROR_STATE);
    CHECK_RESULT(gl_session_set_scalp(session, points, (GRID + 1) * (GRID + 1), counts, GRID * GRID, indices, k - 1, uvs),
                 GL_ERROR_INVALID_ARGUMENT);
    CHECK_OK(gl_session_set_scalp(session, points, (GRID + 1) * (GRID + 1), counts, GRID * GRID, indices, (size_t)k, uvs));
    CHECK_OK(gl_session_scalp_size(session, &points_n, &triangles));
    CHECK(points_n == (GRID + 1) * (GRID + 1) && triangles == 2 * GRID * GRID);
    CHECK_OK(gl_session_evaluate(session, NULL, &strands));
    CHECK(gl_strands_strand_count(strands) > 0);
    checkConsistent(strands);
    {
        /* The default graph's HairColor: a per-strand linear RGB; the grid's UVs at the roots. */
        const float* values = NULL;
        int32_t scope = -1, components = 0;
        CHECK_OK(gl_strands_attribute(strands, "color", &values, &scope, &components));
        CHECK(values != NULL && scope == GL_SCOPE_STRAND && components == 3);
        CHECK_OK(gl_strands_attribute(strands, "rootUV", &values, &scope, &components));
        CHECK(values != NULL && scope == GL_SCOPE_STRAND && components == 2);
        if (values) CHECK(values[0] >= 0.0f && values[0] <= 1.0f && values[1] >= 0.0f && values[1] <= 1.0f);
    }

    /* The scalp moves up by 1: the same strands, every point 1 higher. */
    memcpy(moved, points, sizeof points);
    for (i = 0; i < (GRID + 1) * (GRID + 1); ++i) moved[3 * i + 1] += 1.0f;
    CHECK_RESULT(gl_session_update_scalp_points(session, moved, 3, NULL), GL_ERROR_INVALID_ARGUMENT);
    CHECK_OK(gl_session_update_scalp_points(session, moved, (GRID + 1) * (GRID + 1), points));
    CHECK_OK(gl_session_set_frame(session, 2.0));
    CHECK_OK(gl_session_evaluate(session, NULL, &lifted));
    CHECK(gl_strands_strand_count(lifted) == gl_strands_strand_count(strands));
    if (gl_strands_point_count(lifted) == gl_strands_point_count(strands) && gl_strands_point_count(strands)) {
        const float* a = gl_strands_positions(strands);
        const float* b = gl_strands_positions(lifted);
        CHECK(fabsf(b[1] - a[1] - 1.0f) < 1e-4f);
    }

    /* Half the density: fewer strands. A struct without its size is refused. */
    gl_eval_options_init(&options);
    options.density = 0.5f;
    CHECK_OK(gl_session_evaluate(session, &options, &thinned));
    CHECK(gl_strands_strand_count(thinned) < gl_strands_strand_count(lifted));
    options.struct_size = 0;
    {
        gl_strands* none = (gl_strands*)(void*)&options;
        CHECK_RESULT(gl_session_evaluate(session, &options, &none), GL_ERROR_INVALID_ARGUMENT);
        CHECK(none == NULL);
    }

    gl_strands_release(thinned);
    gl_strands_release(lifted);
    gl_strands_release(strands);
    gl_session_destroy(session);
}

/* Node upgrades. A node with its operator's newest version stays as it is, and an unknown node is
 * an error. With the test's version-2 operator TestCApiLift (capi_versioned_op.cpp, in the build
 * that links the C API statically: GROOMLAB_CAPI_TEST_LIFT): a node kept at version 1 is raised,
 * then evaluates as new nodes do (its strands 1 higher). */
static void testUpgrade(void) {
    gl_session* session = NULL;
    char* before = NULL;
    char* after = NULL;
    int32_t upgraded = -1;

    CHECK_OK(gl_session_create(&session));
    CHECK_OK(gl_session_load_file(session, compatGroom()));
    CHECK_OK(gl_session_get_graph_json(session, &before));
    CHECK_OK(gl_session_upgrade_node(session, "clump", &upgraded));
    CHECK(upgraded == 0);
    CHECK_OK(gl_session_upgrade_node(session, "scatter", NULL));
    CHECK_OK(gl_session_get_graph_json(session, &after));
    CHECK(before && after && strcmp(before, after) == 0);
    gl_free(before);
    gl_free(after);
    upgraded = -1;
    CHECK_RESULT(gl_session_upgrade_node(session, "nobody", &upgraded), GL_ERROR_NOT_FOUND);
    CHECK(upgraded == 0 && hasText(gl_last_error(), "nobody"));
    CHECK_RESULT(gl_session_upgrade_node(session, NULL, &upgraded), GL_ERROR_INVALID_ARGUMENT);
    CHECK_RESULT(gl_session_upgrade_node(NULL, "clump", &upgraded), GL_ERROR_INVALID_ARGUMENT);

#ifdef GROOMLAB_CAPI_TEST_LIFT
    {
        /* The default graph with a TestCApiLift node, saved at version 1, before its output. */
        static const char kGraph[] =
            "{\"nodes\": [{\"name\": \"scalp\", \"type\": \"Scalp\"}, {\"name\": \"guides\", \"type\": \"Guides\"},"
            " {\"name\": \"scatter\", \"type\": \"Scatter\"}, {\"name\": \"interpolate\", \"type\": \"Interpolate\"},"
            " {\"name\": \"lift\", \"type\": \"TestCApiLift\", \"version\": 1, \"params\": {}},"
            " {\"name\": \"output\", \"type\": \"Output\"}],"
            " \"connections\": [{\"from\": \"scalp\", \"to\": \"scatter\", \"input\": \"scalp\"},"
            " {\"from\": \"scatter\", \"to\": \"interpolate\", \"input\": \"strands\"},"
            " {\"from\": \"guides\", \"to\": \"interpolate\", \"input\": \"guides\"},"
            " {\"from\": \"scalp\", \"to\": \"interpolate\", \"input\": \"scalp\"},"
            " {\"from\": \"interpolate\", \"to\": \"lift\", \"input\": \"strands\"},"
            " {\"from\": \"lift\", \"to\": \"output\", \"input\": \"strands\"}],"
            " \"output\": \"output\"}";
        gl_strands* old = NULL;
        gl_strands* raised = NULL;
        char* spec = NULL;
        double lift = 0.0;
        size_t i;

        CHECK_OK(gl_session_set_graph_json(session, kGraph));
        CHECK_OK(gl_session_evaluate(session, NULL, &old));
        CHECK(gl_strands_point_count(old) > 0);
        CHECK_OK(gl_session_node_spec_json(session, "lift", &spec));
        CHECK(hasText(spec, "\"nodeVersion\":1") && hasText(spec, "\"outdated\":true"));
        gl_free(spec);

        CHECK_OK(gl_session_upgrade_node(session, "lift", &upgraded));
        CHECK(upgraded == 1);
        CHECK_OK(gl_session_node_spec_json(session, "lift", &spec));
        CHECK(hasText(spec, "\"nodeVersion\":2") && hasText(spec, "\"outdated\":false"));
        gl_free(spec);
        CHECK_OK(gl_session_get_graph_json(session, &after));
        CHECK(hasText(after, "\"lift\":1.0"));  /* the param version 2 added, written */
        gl_free(after);
        CHECK_OK(gl_session_get_param_float(session, "lift", "lift", &lift));
        CHECK(lift == 1.0);
        CHECK_OK(gl_session_evaluate(session, NULL, &raised));
        CHECK(gl_strands_point_count(raised) == gl_strands_point_count(old));
        if (gl_strands_point_count(raised) == gl_strands_point_count(old)) {
            const float* a = gl_strands_positions(old);
            const float* b = gl_strands_positions(raised);
            int same = 1;
            for (i = 0; i < gl_strands_point_count(old); ++i) {
                same &= b[3 * i] == a[3 * i] && fabsf(b[3 * i + 1] - a[3 * i + 1] - 1.0f) < 1e-5f && b[3 * i + 2] == a[3 * i + 2];
            }
            CHECK(same);
        }
        CHECK_OK(gl_session_upgrade_node(session, "lift", &upgraded));
        CHECK(upgraded == 0);
        gl_strands_release(raised);
        gl_strands_release(old);
    }
#endif
    gl_session_destroy(session);
}

static void testErrors(void) {
    gl_session* session = NULL;
    gl_strands* strands = NULL;
    char* text = NULL;
    size_t nodes = 0;
    static const char kNotAGroom[] = "{\"format\": \"something.else\"}";

    CHECK_RESULT(gl_session_create(NULL), GL_ERROR_INVALID_ARGUMENT);
    CHECK_RESULT(gl_session_evaluate(NULL, NULL, &strands), GL_ERROR_INVALID_ARGUMENT);
    CHECK(strands == NULL);
    CHECK_OK(gl_session_create(&session));
    CHECK_OK(gl_session_node_count(session, &nodes));
    CHECK(nodes > 0);  /* the default graph */
    CHECK_OK(gl_session_evaluate(session, NULL, &strands));  /* no scalp: no strands, no error */
    CHECK(strands != NULL && gl_strands_strand_count(strands) == 0 && gl_strands_positions(strands) == NULL);
    CHECK(gl_strands_offsets(strands) != NULL && gl_strands_offsets(strands)[0] == 0);
    gl_strands_release(strands);

    CHECK_RESULT(gl_session_load_file(session, "no/such/file.groom"), GL_ERROR_IO);
    CHECK(hasText(gl_last_error(), "no/such/file.groom"));
    CHECK_RESULT(gl_session_load_memory(session, "not json", 8, NULL), GL_ERROR_PARSE);
    CHECK(strlen(gl_last_error()) > 0);
    CHECK_RESULT(gl_session_load_memory(session, kNotAGroom, sizeof kNotAGroom - 1, NULL), GL_ERROR_PARSE);
    CHECK_RESULT(gl_session_set_frames_per_second(session, 0.0), GL_ERROR_INVALID_ARGUMENT);
    CHECK_RESULT(gl_session_save_file(session, "no/such/folder/out.groom"), GL_ERROR_IO);
    /* The session is as it was: the default graph, still usable. */
    CHECK_OK(gl_session_get_graph_json(session, &text));
    CHECK(hasText(text, "\"Scatter\""));
    CHECK(strcmp(gl_last_error(), "") == 0);
    gl_free(text);
    gl_session_destroy(session);
    gl_session_destroy(NULL);
    gl_strands_release(NULL);
    gl_free(NULL);
    CHECK(gl_strands_strand_count(NULL) == 0 && gl_strands_positions(NULL) == NULL);
}

/* Time by every host's rule, and the groom turned into another up axis. */
static void testAxesAndTime(void) {
    gl_session* session = NULL;
    char* axis = NULL;
    char* data = NULL;
    double seconds = 0.0, fps = 0.0;
    CHECK(gl_frame_to_seconds(48.0, 24.0) == 2.0);
    CHECK(gl_frame_to_seconds(7.0, 23.976) == 7.0 * 1001.0 / 24000.0);
    CHECK(gl_frame_to_seconds(7.0, 24000.0 / 1001.0) == gl_frame_to_seconds(7.0, 23.976));
    CHECK(gl_frame_to_seconds(48.0, 0.0) == 2.0);
    CHECK_OK(gl_session_create(&session));
    CHECK_OK(gl_session_load_file(session, compatGroom()));
    CHECK_OK(gl_session_set_frames_per_second(session, 29.97));
    CHECK_OK(gl_session_get_frames_per_second(session, &fps));
    CHECK(fps == 30000.0 / 1001.0);
    CHECK_OK(gl_session_set_frame(session, 11.0));
    CHECK_OK(gl_session_get_time(session, &seconds));
    CHECK(seconds == 11.0 * 1001.0 / 30000.0);
    CHECK_OK(gl_session_get_up_axis(session, &axis));
    CHECK(axis != NULL && strcmp(axis, "Y") == 0);
    gl_free(axis);
    CHECK_OK(gl_session_set_up_axis(session, "Z"));
    CHECK_OK(gl_session_get_up_axis(session, &axis));
    CHECK(axis != NULL && strcmp(axis, "Z") == 0);
    gl_free(axis);
    CHECK_OK(gl_session_get_time(session, &seconds));
    CHECK(seconds == 11.0 * 1001.0 / 30000.0); /* the time stays */
    CHECK_OK(gl_session_save_memory(session, &data, NULL));
    CHECK(hasText(data, "\"upAxis\":\"Z\""));
    gl_free(data);
    CHECK_RESULT(gl_session_set_up_axis(session, "X"), GL_ERROR_INVALID_ARGUMENT);
    CHECK_RESULT(gl_session_get_up_axis(session, NULL), GL_ERROR_INVALID_ARGUMENT);
    gl_session_destroy(session);
}

int main(void) {
    testVersion();
    testAxesAndTime();
    testOperators();
    testLoadEvaluateSave();
    testParams();
    testScalpFromPolygons();
    testUpgrade();
    testErrors();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("groomlab C API: all checks passed\n");
    return 0;
}
