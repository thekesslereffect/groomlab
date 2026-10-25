# SPDX-License-Identifier: Apache-2.0
groomlab_fetch_googletest()
include(GoogleTest)

add_executable(groomlab_tests
    engine/core/test_async_evaluation.cpp
    engine/core/test_attributes.cpp
    engine/core/test_brush.cpp
    engine/core/test_brush_catalog.cpp
    engine/core/test_brush_pack.cpp
    engine/core/test_budgets.cpp
    engine/core/test_budgets_ops.cpp
    engine/core/test_budgets_paint.cpp
    engine/core/test_budgets_stroke.cpp
    engine/core/test_camera_solve.cpp
    engine/core/test_reference_edit.cpp
    engine/core/test_reference_guides.cpp
    engine/core/test_color_patterns.cpp
    engine/core/test_hair_color.cpp
    engine/core/test_renderman_look.cpp
    engine/core/test_compat.cpp
    engine/core/test_compound.cpp
    engine/core/test_deform.cpp
    engine/core/test_continuity.cpp
    engine/core/test_determinism.cpp
    engine/core/test_distance_field.cpp
    engine/core/test_follow.cpp
    engine/core/test_display.cpp
    engine/core/test_strand_lod.cpp
    engine/core/test_expression.cpp
    engine/core/test_feathers.cpp
    engine/core/test_feather_cards.cpp
    engine/core/test_geometry.cpp
    engine/core/test_golden.cpp
    engine/core/test_surface_paths.cpp
    engine/core/test_graph.cpp
    engine/core/test_graph_pins.cpp
    engine/core/test_grooming.cpp
    engine/core/test_hierarchy.cpp
    engine/core/test_tubes.cpp
    engine/core/test_incremental.cpp
    engine/core/test_keyed_params.cpp
    engine/core/test_value_inputs.cpp
    engine/core/test_cards.cpp
    engine/core/test_card_separation.cpp
    engine/core/test_card_maps.cpp
    engine/core/test_fields.cpp
    engine/core/test_game_exports.cpp
    engine/core/test_cinematic.cpp
    engine/core/test_lod.cpp
    engine/core/test_correctives.cpp
    engine/core/test_flow.cpp
    engine/core/test_layers.cpp
    engine/core/test_locality.cpp
    engine/core/test_sculpt.cpp
    engine/core/test_sculpt_node.cpp
    engine/core/test_session.cpp
    engine/core/test_shapes.cpp
    engine/core/test_simulation.cpp
    engine/core/test_sim_attributes.cpp
    engine/core/test_transfer.cpp
    engine/core/test_transfer_methods.cpp
    engine/core/test_transfer_markers_edit.cpp
    engine/core/test_look.cpp
    engine/core/test_maps.cpp
    engine/core/test_motion.cpp
    engine/core/test_node_common.cpp
    engine/core/test_node_library.cpp
    engine/core/test_script_node.cpp
    engine/core/test_operators.cpp
    engine/core/test_param_use.cpp
    engine/core/test_parity_nodes.cpp
    engine/core/test_parts.cpp
    engine/core/test_pipeline.cpp
    engine/core/test_plume.cpp
    engine/core/test_presets.cpp
    engine/core/test_ptex.cpp
    engine/core/test_preview_invariance.cpp
    engine/core/test_rachis.cpp
    engine/core/test_random.cpp
    engine/core/test_regions.cpp
    engine/core/test_reference.cpp
    engine/core/test_render_overrides.cpp
    engine/core/test_robustness.cpp
    engine/core/test_scale_export.cpp
    engine/core/test_scale_invariance.cpp
    engine/core/test_scale_with_scalp.cpp
    engine/core/test_scales.cpp
    engine/core/test_scalp_paint.cpp
    engine/core/test_semver.cpp
    engine/core/test_strand_geometry.cpp
    engine/core/test_strand_set.cpp
    engine/core/test_stroke.cpp
    engine/core/test_scalp_pieces.cpp
    engine/core/test_subdivision.cpp
    engine/core/test_self_collide.cpp
    engine/core/test_static.cpp
    engine/core/test_tie.cpp
    engine/core/test_usd_layout.cpp
    engine/core/test_vrscene_writer.cpp
    engine/core/test_wet.cpp
    engine/core/test_example_node.cpp
    # The new-node template, built as a test's operator ("TestExample": test_example_node.cpp); its
    # "internal.h" is example_node/internal.h, which gives it op::kExample.
    ${PROJECT_SOURCE_DIR}/tools/dev/new_node/example_op.cpp
)
target_link_libraries(groomlab_tests PRIVATE groomlab::core groomlab::warnings GTest::gtest_main nlohmann_json::nlohmann_json)
# stb_image's declarations (the core holds its code): the card map tests read their files back.
target_include_directories(groomlab_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
if(WIN32)
    target_link_libraries(groomlab_tests PRIVATE psapi)  # peak memory in the budget tests
else()
    # `if (c) EXPECT_...(...) << ...;`: GoogleTest's macros end in an if/else of their own.
    target_compile_options(groomlab_tests PRIVATE -Wno-dangling-else)
endif()
groomlab_path_literal(_test_data "${CMAKE_CURRENT_SOURCE_DIR}/data")
groomlab_path_literal(_shipped_presets "${PROJECT_SOURCE_DIR}/hosts/maya/presets")
target_compile_definitions(groomlab_tests PRIVATE "GROOMLAB_TEST_DATA=${_test_data}"
                           "GROOMLAB_SHIPPED_PRESETS=${_shipped_presets}")
groomlab_precompile(groomlab_tests <gtest/gtest.h> <nlohmann/json.hpp>)
# These define macros before their includes (_CRT_SECURE_NO_WARNINGS, windows.h's): a precompiled
# header would come first, so they compile without it, as written.
set_source_files_properties(engine/core/test_budgets.cpp engine/core/test_budgets_paint.cpp
    engine/core/test_budgets_stroke.cpp engine/core/test_compat.cpp engine/core/test_golden.cpp
    engine/core/test_self_collide.cpp engine/core/test_static.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# The USD layout test reads its spec page (web/content/docs/usd-layout.mdx); the definition on this
# file only, so adding it rebuilds nothing else.
groomlab_path_literal(_source_root "${PROJECT_SOURCE_DIR}")
set_source_files_properties(engine/core/test_usd_layout.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "GROOMLAB_SOURCE_ROOT=${_source_root}")
set_source_files_properties(${PROJECT_SOURCE_DIR}/tools/dev/new_node/example_op.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/engine/core/example_node")
if(TARGET groomlab_alembic)
    target_sources(groomlab_tests PRIVATE engine/io/test_alembic.cpp engine/io/test_import_curves.cpp engine/io/test_sim_cache.cpp engine/io/test_time_determinism.cpp)
    target_link_libraries(groomlab_tests PRIVATE groomlab::alembic Alembic::Alembic)  # tests read files back directly
endif()

# Copy runtime DLLs (e.g. Maya's tbb.dll) next to the test binary so test
# discovery and ctest runs work without touching PATH.
if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    add_custom_command(TARGET groomlab_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:groomlab_tests> $<TARGET_FILE_DIR:groomlab_tests>
        COMMAND_EXPAND_LISTS)
endif()

# The screen-space level of detail's picture test (strand_lod.h, docs/research/clump_lod.md): the
# research harness (tools/research/clump_lod) in --check mode, hair_basics on the app's head at
# 1M strands (as the research), the hosts' drawing against a physical reference (ribbons of the true width, 64
# samples a pixel) at the four framings: its ink within 2% of the reference's, its error no worse
# than every strand's as drawn before. A few minutes: label "pictures" alone (ctest -L pictures).
add_executable(groomlab_lod_pictures ${PROJECT_SOURCE_DIR}/tools/research/clump_lod/bench_clump_lod.cpp)
target_link_libraries(groomlab_lod_pictures PRIVATE groomlab::core)
target_compile_definitions(groomlab_lod_pictures PRIVATE "RESEARCH_DATA=\"${PROJECT_SOURCE_DIR}/tests/data\""
                           "RESEARCH_PRESETS=\"${PROJECT_SOURCE_DIR}/hosts/maya/presets\"")
if(MSVC)
    target_compile_options(groomlab_lod_pictures PRIVATE /W3 /permissive-)
    target_compile_definitions(groomlab_lod_pictures PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    add_custom_command(TARGET groomlab_lod_pictures POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:groomlab_lod_pictures> $<TARGET_FILE_DIR:groomlab_lod_pictures>
        COMMAND_EXPAND_LISTS)
endif()
add_test(NAME core.lod_pictures COMMAND groomlab_lod_pictures --check --strands 1000000 --size 512
         --out "${CMAKE_CURRENT_BINARY_DIR}/lod_pictures")
set_tests_properties(core.lod_pictures PROPERTIES LABELS "pictures;core" TIMEOUT 1800 RUN_SERIAL TRUE)

# Test tiers (ctest -L, docs/DEVELOPING.md section 2): `quick` (no windows: core, bindings and the
# mayapy suites), `gui` (one Maya window running every GUI suite), `arnold` (MtoA and the
# procedural), `renderman` (the RenderMan procedural in prman). No label filter runs all of them. Every test also has the label of the area it
# tests: `core`, `io` (Alembic, groom scenes), `gpu`, `python`, `maya`, `app`
# (tools/dev/test_changed.py picks them from the files a branch changed).
#
# gtest_discover_tests flattens a list given to PROPERTIES (LABELS "quick;core" would set LABELS
# quick and a property named core), so discovered tests get their labels from a file ctest
# includes after the discovery's (groomlab_label_discovered): the tests it found by name.
function(groomlab_label_discovered test_list)
    set(file "${CMAKE_CURRENT_BINARY_DIR}/${test_list}_labels.cmake")
    list(JOIN ARGN ";" labels)
    file(WRITE "${file}" "if(${test_list})\n  set_tests_properties(\${${test_list}} PROPERTIES LABELS \"${labels}\")\nendif()\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${file}")
endfunction()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
    set(_io_suites "Alembic.*:ImportCurves.*:SimulationCache.*")
    # The Budget and Performance tests time parallel loops: alone, not beside other tests (ctest -j).
    gtest_discover_tests(groomlab_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "-Budget.*:Performance.*:${_io_suites}"
                         TEST_LIST groomlab_core_tests PROPERTIES LABELS quick)
    gtest_discover_tests(groomlab_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "${_io_suites}"
                         TEST_LIST groomlab_io_tests PROPERTIES LABELS quick)
    gtest_discover_tests(groomlab_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "Budget.*:Performance.*"
                         TEST_LIST groomlab_timing_tests PROPERTIES LABELS quick RUN_SERIAL TRUE)
    groomlab_label_discovered(groomlab_io_tests quick io)
    groomlab_label_discovered(groomlab_timing_tests quick core)
else()
    gtest_discover_tests(groomlab_tests DISCOVERY_MODE PRE_TEST TEST_LIST groomlab_core_tests PROPERTIES LABELS quick)
endif()
groomlab_label_discovered(groomlab_core_tests quick core)

# tools/dev/new_node.py finds every place a new node goes (it changes nothing with --dry-run).
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_tool_python "${Python3_EXECUTABLE}")
if(NOT _tool_python)
    set(_tool_python "${GROOMLAB_PYTHON_EXECUTABLE}")
endif()
if(_tool_python)
    add_test(NAME tools.new_node_dry_run
        COMMAND "${_tool_python}" "${PROJECT_SOURCE_DIR}/tools/dev/new_node.py" ExampleDryRun --category look --gpu --dry-run)
    set_tests_properties(tools.new_node_dry_run PROPERTIES LABELS "quick;core")
endif()

# engine/ui/*.json, the UI structure every host reads: the schema, the references, and (under the
# Python the core module was built for) the shortcut ids and tools against the core.
if(GROOMLAB_BUILD_PYTHON AND TARGET groomlab_core_py)
    add_test(NAME ui.definitions
        COMMAND ${GROOMLAB_PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/engine/ui/check_ui_definitions.py")
    set_tests_properties(ui.definitions PROPERTIES LABELS "quick;core"
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:groomlab_core_py>")
elseif(_tool_python)
    add_test(NAME ui.definitions
        COMMAND "${_tool_python}" "${CMAKE_CURRENT_SOURCE_DIR}/engine/ui/check_ui_definitions.py")
    set_tests_properties(ui.definitions PROPERTIES LABELS "quick;core")
endif()

# groomlab-cli's MCP server (engine/cli/mcp_server.h) played a scripted agent session as a client
# would: a subprocess on stdin/stdout, files in a folder of its own as the server's root.
if(_tool_python AND TARGET groomlab-cli AND TARGET groomlab_alembic)
    add_test(NAME cli.mcp_session
        COMMAND "${_tool_python}" "${CMAKE_CURRENT_SOURCE_DIR}/engine/cli/test_mcp.py"
                "$<TARGET_FILE:groomlab-cli>" "${CMAKE_CURRENT_BINARY_DIR}/mcp_session")
    set_tests_properties(cli.mcp_session PROPERTIES LABELS "quick;core" TIMEOUT 300)
endif()

# The C API library's parallel backend (engine/core/src/parallel_pool.cpp, the pool of the
# library's own threads): against groomlab_core_private, the core with that backend.
if(TARGET groomlab_core_private)
    add_executable(groomlab_pool_tests engine/core/test_parallel_pool.cpp)
    target_link_libraries(groomlab_pool_tests PRIVATE groomlab_core_private groomlab::warnings GTest::gtest_main)
    gtest_discover_tests(groomlab_pool_tests DISCOVERY_MODE PRE_TEST TEST_LIST groomlab_pool_test_list PROPERTIES LABELS quick)
    groomlab_label_discovered(groomlab_pool_test_list quick core)
endif()

# The GPU look preview's kernels against the CPU operators (engine/gpu/, look_engine.h): on a device of
# its own, skipped without one. Windows only: elsewhere the engine has no backend yet (engine/gpu).
if(TARGET groomlab_gpu AND WIN32)
    add_executable(groomlab_gpu_tests engine/gpu/test_look_engine.cpp engine/gpu/test_line_frames.cpp)
    target_link_libraries(groomlab_gpu_tests PRIVATE groomlab::gpu groomlab::core groomlab::warnings GTest::gtest_main
                          nlohmann_json::nlohmann_json)
    target_compile_definitions(groomlab_gpu_tests PRIVATE "GROOMLAB_SHIPPED_PRESETS=${_shipped_presets}")  # hair_basics
    if(MSVC)
        target_compile_options(groomlab_gpu_tests PRIVATE /bigobj)  # (test_look_engine.cpp: past the default's sections)
    endif()
    if(GROOMLAB_TBB_RUNTIME)
        add_custom_command(TARGET groomlab_gpu_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:groomlab_gpu_tests> $<TARGET_FILE_DIR:groomlab_gpu_tests>
            COMMAND_EXPAND_LISTS)
    endif()
    gtest_discover_tests(groomlab_gpu_tests DISCOVERY_MODE PRE_TEST TEST_LIST groomlab_gpu_test_list PROPERTIES LABELS quick)
    groomlab_label_discovered(groomlab_gpu_test_list quick gpu)

    # The GPU look engine's benchmark (numbers, not checks): label "bench" alone, and it skips
    # unless GROOMLAB_BENCH=1 (GROOMLAB_BENCH_COUNTS, GROOMLAB_BENCH_OUT: bench_look_engine.cpp).
    add_executable(groomlab_gpu_bench engine/gpu/bench_look_engine.cpp)
    target_link_libraries(groomlab_gpu_bench PRIVATE groomlab::gpu groomlab::core groomlab::warnings GTest::gtest_main
                          nlohmann_json::nlohmann_json)
    target_compile_definitions(groomlab_gpu_bench PRIVATE "GROOMLAB_SHIPPED_PRESETS=${_shipped_presets}")
    if(GROOMLAB_TBB_RUNTIME)
        add_custom_command(TARGET groomlab_gpu_bench POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:groomlab_gpu_bench> $<TARGET_FILE_DIR:groomlab_gpu_bench>
            COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME gpu.bench COMMAND groomlab_gpu_bench)
    set_tests_properties(gpu.bench PROPERTIES LABELS "bench;gpu" RUN_SERIAL TRUE TIMEOUT 1800)
endif()

# The C API (engine/capi): a test compiled as C against the shared library alone, and the C++
# wrapper's against the core's own GroomSession (bit-exact results: the library's own threads give
# what the core's TBB does). Each gets the library beside it.
if(TARGET groomlab_capi)
    enable_language(C)
    groomlab_path_literal(_compat_groom "${CMAKE_CURRENT_SOURCE_DIR}/data/compat/v1/groom.groom")

    add_executable(groomlab_capi_c_test engine/capi/test_capi.c)
    set_target_properties(groomlab_capi_c_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(groomlab_capi_c_test PRIVATE groomlab::capi)
    target_compile_definitions(groomlab_capi_c_test PRIVATE "GROOMLAB_CAPI_COMPAT_GROOM=${_compat_groom}")
    if(MSVC)
        target_compile_options(groomlab_capi_c_test PRIVATE /W4 /WX)
    else()
        target_compile_options(groomlab_capi_c_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(groomlab_capi_c_test PRIVATE m)
    endif()
    add_test(NAME capi.c COMMAND groomlab_capi_c_test)
    set_tests_properties(capi.c PROPERTIES LABELS "quick;core")

    add_executable(groomlab_capi_tests engine/capi/test_capi_wrapper.cpp)
    target_link_libraries(groomlab_capi_tests PRIVATE groomlab::capi groomlab::core groomlab::warnings GTest::gtest_main
                          nlohmann_json::nlohmann_json)
    target_compile_definitions(groomlab_capi_tests PRIVATE "GROOMLAB_TEST_DATA=${_test_data}"
                               "GROOMLAB_SHIPPED_PRESETS=${_shipped_presets}")

    foreach(_capi_test groomlab_capi_c_test groomlab_capi_tests)
        if(WIN32)
            add_custom_command(TARGET ${_capi_test} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${_capi_test}> $<TARGET_RUNTIME_DLLS:groomlab_capi>
                        $<TARGET_FILE_DIR:${_capi_test}>
                COMMAND_EXPAND_LISTS)
        endif()
    endforeach()
    gtest_discover_tests(groomlab_capi_tests DISCOVERY_MODE PRE_TEST TEST_LIST groomlab_capi_test_list PROPERTIES LABELS quick)
    groomlab_label_discovered(groomlab_capi_test_list quick core)

    # Node upgrades on a node that is outdated need an operator at version 2, which no built-in
    # has yet: capi_versioned_op.cpp registers one in the core the test links, so these tests link
    # the C API in statically (GROOMLAB_C_STATIC) beside it. The C test again, and the wrapper's.
    add_library(groomlab_capi_static STATIC ${PROJECT_SOURCE_DIR}/engine/capi/src/groomlab_c.cpp)
    target_include_directories(groomlab_capi_static PUBLIC ${PROJECT_SOURCE_DIR}/engine/capi/include)
    target_compile_definitions(groomlab_capi_static PUBLIC GROOMLAB_C_STATIC PRIVATE GROOMLAB_C_BUILD)
    target_link_libraries(groomlab_capi_static PUBLIC groomlab::core PRIVATE nlohmann_json::nlohmann_json groomlab::warnings)
    add_library(groomlab_capi_test_ops OBJECT engine/capi/capi_versioned_op.cpp)
    target_link_libraries(groomlab_capi_test_ops PRIVATE groomlab::core groomlab::warnings)

    add_executable(groomlab_capi_c_static_test engine/capi/test_capi.c $<TARGET_OBJECTS:groomlab_capi_test_ops>)
    set_target_properties(groomlab_capi_c_static_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(groomlab_capi_c_static_test PRIVATE groomlab_capi_static)
    target_compile_definitions(groomlab_capi_c_static_test PRIVATE "GROOMLAB_CAPI_COMPAT_GROOM=${_compat_groom}"
                               GROOMLAB_CAPI_TEST_LIFT)
    if(MSVC)
        target_compile_options(groomlab_capi_c_static_test PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4 /WX>)
    else()
        target_compile_options(groomlab_capi_c_static_test PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror>)
        target_link_libraries(groomlab_capi_c_static_test PRIVATE m)
    endif()
    add_test(NAME capi.c_static COMMAND groomlab_capi_c_static_test)
    set_tests_properties(capi.c_static PROPERTIES LABELS "quick;core")

    add_executable(groomlab_capi_upgrade_tests engine/capi/test_capi_upgrade.cpp $<TARGET_OBJECTS:groomlab_capi_test_ops>)
    target_link_libraries(groomlab_capi_upgrade_tests PRIVATE groomlab_capi_static groomlab::warnings GTest::gtest_main
                          nlohmann_json::nlohmann_json)
    target_compile_definitions(groomlab_capi_upgrade_tests PRIVATE "GROOMLAB_TEST_DATA=${_test_data}")
    foreach(_capi_test groomlab_capi_c_static_test groomlab_capi_upgrade_tests)
        if(WIN32)
            add_custom_command(TARGET ${_capi_test} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${_capi_test}> $<TARGET_FILE_DIR:${_capi_test}>
                COMMAND_EXPAND_LISTS)
        endif()
    endforeach()
    gtest_discover_tests(groomlab_capi_upgrade_tests DISCOVERY_MODE PRE_TEST TEST_LIST groomlab_capi_upgrade_test_list
                         PROPERTIES LABELS quick)
    groomlab_label_discovered(groomlab_capi_upgrade_test_list quick core)

    # The SDK samples (engine/capi/samples) as a bridge runs them: a shipped groom file to OBJ
    # polylines, the default graph on an OBJ scalp, and the C sample's counts.
    set(_compat_v1 "${CMAKE_CURRENT_SOURCE_DIR}/data/compat/v1/groom.groom")
    add_test(NAME capi.sample.groom_to_obj
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:groom_to_obj> -DGROOM=${_compat_v1} -DFRAME=1 -DSTRANDS=2210
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sample_compat_v1.obj -P ${CMAKE_CURRENT_SOURCE_DIR}/engine/capi/check_groom_to_obj.cmake)
    add_test(NAME capi.sample.groom_to_obj_scalp
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:groom_to_obj> -DSCALP=${CMAKE_CURRENT_SOURCE_DIR}/data/app/head_scalp.obj -DDENSITY=0.1
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sample_head_scalp.obj -P ${CMAKE_CURRENT_SOURCE_DIR}/engine/capi/check_groom_to_obj.cmake)
    add_test(NAME capi.sample.groom_counts COMMAND groom_counts ${_compat_v1} 1)
    set_tests_properties(capi.sample.groom_counts PROPERTIES PASS_REGULAR_EXPRESSION "^2210 strands, [0-9]+ points")
    set_tests_properties(capi.sample.groom_to_obj capi.sample.groom_to_obj_scalp capi.sample.groom_counts
                         PROPERTIES LABELS "quick;core")
    # The docs' function reference (web/content/docs/c-api.mdx) against the header's comments.
    if(GROOMLAB_PYTHON_EXECUTABLE)
        add_test(NAME capi.reference_docs
            COMMAND ${GROOMLAB_PYTHON_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/dev/capi_reference.py" --check)
        set_tests_properties(capi.reference_docs PROPERTIES LABELS "quick;core")
    endif()
endif()

# Python binding tests run under the same interpreter the module was built for.
if(GROOMLAB_BUILD_PYTHON AND TARGET groomlab_core_py)
    add_test(NAME python.groomlab_core
        COMMAND ${GROOMLAB_PYTHON_EXECUTABLE} -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/engine/python" -v)
    # GROOMLAB_CLI: test_groom_session.py checks a session's strands against groomlab-cli's.
    set_tests_properties(python.groomlab_core PROPERTIES
        ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:groomlab_core_py>;GROOMLAB_CLI=$<$<TARGET_EXISTS:groomlab-cli>:$<TARGET_FILE:groomlab-cli>>"
        LABELS "quick;python")
endif()

# groomlab-helper (hosts/blender/helper) driven as the Blender add-on drives it, without Blender:
# under Blender's own Python when Blender is installed (numpy comes with it), else a Python 3 with
# numpy (GROOMLAB_BLENDER_PYTHON picks one). Against groomlab-cli's strands of the files it saves.
if(TARGET groomlab-helper AND TARGET groomlab-cli)
    set(GROOMLAB_BLENDER_PYTHON "" CACHE FILEPATH "A Python 3 with numpy for blender.helper (default: Blender's own, else Python 3)")
    set(_blender_python "${GROOMLAB_BLENDER_PYTHON}")
    if(NOT _blender_python)
        file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _program_files)
        file(GLOB _blender_pythons
             "${_program_files}/Blender Foundation/Blender */*/python/bin/python.exe"
             "/Applications/Blender.app/Contents/Resources/*/python/bin/python3*"
             "/usr/share/blender/*/python/bin/python3*")
        if(_blender_pythons)
            list(SORT _blender_pythons COMPARE NATURAL ORDER DESCENDING)
            list(GET _blender_pythons 0 _blender_python)
        else()
            find_package(Python3 COMPONENTS Interpreter QUIET)
            set(_blender_python "${Python3_EXECUTABLE}")
        endif()
    endif()
    set(_blender_numpy 1)
    if(_blender_python)
        execute_process(COMMAND "${_blender_python}" -c "import numpy" RESULT_VARIABLE _blender_numpy OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(_blender_numpy EQUAL 0)
        add_test(NAME blender.helper
            COMMAND "${_blender_python}" -B "${CMAKE_CURRENT_SOURCE_DIR}/hosts/blender/test_helper.py")
        # The tools' strokes are replayed by the core's own BrushTool through the Python bindings
        # (stroke_reference.py, under the Python they were built for) when they are built.
        set(_helper_reference_env "")
        if(GROOMLAB_BUILD_PYTHON AND TARGET groomlab_core_py AND GROOMLAB_PYTHON_EXECUTABLE)
            set(_helper_reference_env
                "GROOMLAB_CORE_PYTHON=${GROOMLAB_PYTHON_EXECUTABLE};GROOMLAB_CORE_PYTHONPATH=$<TARGET_FILE_DIR:groomlab_core_py>")
        endif()
        set_tests_properties(blender.helper PROPERTIES
            LABELS "quick;blender"
            ENVIRONMENT "GROOMLAB_HELPER=$<TARGET_FILE:groomlab-helper>;GROOMLAB_HELPER_TESTHOOKS=$<TARGET_FILE:groomlab-helper-testhooks>;GROOMLAB_CLI=$<TARGET_FILE:groomlab-cli>;GROOMLAB_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data;${_helper_reference_env}")
    else()
        message(STATUS "blender.helper: no Python 3 with numpy (set GROOMLAB_BLENDER_PYTHON): the test is left out")
    endif()

    # The add-on (hosts/blender/addon) in a headless Blender: the build's extension zip installed in
    # a throwaway profile (run_blender_test.py), never the user's own Blender settings.
    set(GROOMLAB_BLENDER_EXE "" CACHE FILEPATH "Blender for the add-on tests (default: the newest under Program Files)")
    set(_blender_exe "${GROOMLAB_BLENDER_EXE}")
    if(NOT _blender_exe)
        file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _program_files)
        file(GLOB _blender_exes "${_program_files}/Blender Foundation/Blender */blender.exe")
        if(_blender_exes)
            list(SORT _blender_exes COMPARE NATURAL ORDER DESCENDING)
            list(GET _blender_exes 0 _blender_exe)
        endif()
    endif()
    if(_blender_exe AND _blender_numpy EQUAL 0 AND GROOMLAB_BLENDER_EXTENSION_ZIP)
        add_test(NAME blender.extension
            COMMAND "${_blender_exe}" --factory-startup --command extension validate "${GROOMLAB_BLENDER_EXTENSION_ZIP}")
        set_tests_properties(blender.extension PROPERTIES LABELS "quick;blender")
        set(_addon_env
            "GROOMLAB_CLI=$<TARGET_FILE:groomlab-cli>;GROOMLAB_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data;GROOMLAB_USER_PRESETS=${CMAKE_CURRENT_BINARY_DIR}/blender_no_user_presets;GROOMLAB_USER_NODES=${CMAKE_CURRENT_BINARY_DIR}/blender_no_user_nodes")
        foreach(_t addon addon_free addon_render addon_graph addon_graph_authoring addon_tools addon_ui addon_pipeline
                   addon_photos addon_vray)
            add_test(NAME blender.${_t}
                COMMAND "${_blender_python}" -B "${CMAKE_CURRENT_SOURCE_DIR}/hosts/blender/run_blender_test.py" "${_blender_exe}"
                        "${GROOMLAB_BLENDER_EXTENSION_ZIP}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/blender/test_${_t}.py")
        endforeach()
        # (addon_graph_authoring: the Graph Editor's authoring; its own node library and preset folders.)
        set_tests_properties(blender.addon blender.addon_render blender.addon_graph_authoring blender.addon_tools
                             blender.addon_pipeline blender.addon_photos PROPERTIES
            LABELS "blender" ENVIRONMENT "${_addon_env}")
        # Final renders in V-Ray for Blender (the groom's V-Ray scene): skipped when V-Ray for Blender is not
        # installed for this Blender (the user's scripts/addons/vray_blender, added read only to the
        # throwaway profile). V-Ray's license is checked out per render: alone.
        set_tests_properties(blender.addon_vray PROPERTIES LABELS "blender;vray" RUN_SERIAL TRUE TIMEOUT 900
                             ENVIRONMENT "${_addon_env}")
        # Free for any use: exports with no key, no notice, Support GroomLab in the menu and About.
        set_tests_properties(blender.addon_free PROPERTIES LABELS "quick;blender" ENVIRONMENT "${_addon_env}")
        # The UI from the shared definitions (engine/ui): every id Blender shows bound, the Groom Tools' operators.
        set_tests_properties(blender.addon_ui PROPERTIES LABELS "blender" ENVIRONMENT "${_addon_env}")
        # The Graph Editor. The test build of the helper has an operator at version 2 for the Upgrade case.
        set_tests_properties(blender.addon_graph PROPERTIES
            LABELS "blender"
            ENVIRONMENT "${_addon_env};GROOMLAB_HELPER_TESTHOOKS=$<TARGET_FILE:groomlab-helper-testhooks>")
    else()
        message(STATUS "blender.addon: no Blender found (set GROOMLAB_BLENDER_EXE): the add-on tests are left out")
    endif()
endif()

# Maya integration test: loads the plugin in mayapy standalone.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.plugin
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_plugin.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
endif()

# Graph API, presets and the graph editor (Qt offscreen).
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.graph_editor
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_graph_editor.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
endif()

# Pipeline: rest mesh, USD export, groom files evaluated by groomlab-cli.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab AND TARGET groomlab-cli AND TARGET groomlab_alembic)
    add_test(NAME maya.pipeline
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_pipeline.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE:groomlab-cli>"
                "${CMAKE_CURRENT_BINARY_DIR}/pipeline")
endif()

# Groom files back into Maya: import, reload in place, save back, the round trips Maya -> .groom ->
# Maya and with the standalone app (run headless when it is built).
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab AND TARGET groomlab-cli AND TARGET groomlab_alembic)
    add_test(NAME maya.groom_io
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_groom_io.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE:groomlab-cli>"
                "${CMAKE_CURRENT_BINARY_DIR}/groom_io"
                "${CMAKE_CURRENT_SOURCE_DIR}/data"
                "$<$<TARGET_EXISTS:groomlab_app>:$<TARGET_FILE:groomlab_app>>")
    # The app's runs keep their settings in the work folder, never the user's.
    set_tests_properties(maya.groom_io PROPERTIES ENVIRONMENT "GROOMLAB_SETTINGS_DIR=${CMAKE_CURRENT_BINARY_DIR}/groom_io/settings")
endif()

# Check for Updates: version order, a check and an install against a fake downloads server on
# localhost (never the real domain), tampered zips refused, developer installs never touched, the
# once-a-day startup check, offline silence.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.updates
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_updates.py"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
endif()

# Catmull-Clark on the scalp: match render from the mesh's Arnold subdivision or smooth preview,
# groom files, our surface against Maya's own smooth mesh.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.subdivision
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_subdivision.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
endif()

# Grooming: guide selection, masks, symmetry, clump paint, hierarchy tools.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.grooming
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_grooming.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
endif()

# The live strand mesh (StrandMesh node, outStrandMesh): a deforming scalp with follow scalp and
# Simulate, a fixed topology, the snapshot matched, Alembic export; ms per frame of 10k tubes.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    add_test(NAME maya.strand_mesh
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_strand_mesh.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "${CMAKE_CURRENT_BINARY_DIR}/strand_mesh")
endif()

# Importing XGen grooms (both flavors) on synthetic scenes made with XGen's API, compared with
# XGen's own strands (pictures in the work dir). Needs Maya's XGen plugin (skipped without it).
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab AND TARGET groomlab_alembic)
    add_test(NAME maya.xgen_import
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_xgen_import.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "${CMAKE_CURRENT_BINARY_DIR}/xgen_import")
endif()

# Importing Yeti grooms without Yeti, on a synthetic scene written in Yeti's format
# (data/yeti/make_scenes.py): the format reader, the mapping, guide layers, textures.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab AND TARGET groomlab_alembic)
    add_test(NAME maya.yeti_import
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_yeti_import.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "${CMAKE_CURRENT_SOURCE_DIR}/data/yeti"
                "${CMAKE_CURRENT_BINARY_DIR}/yeti_import")
endif()

# 1.0 hardening: errors on the node, referencing and moved projects, scene-open time.
if(GROOMLAB_BUILD_MAYA AND TARGET GroomLab AND TARGET groomlab-cli AND TARGET groomlab_alembic)
    add_test(NAME maya.hardening
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_hardening.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE:groomlab-cli>"
                "${CMAKE_CURRENT_BINARY_DIR}/hardening"
                "${CMAKE_CURRENT_SOURCE_DIR}/data/textures/density.pgm")
endif()

# arnold_curves.h against Arnold alone (what the translator and the procedural hand Arnold, and an
# update made while Arnold renders): Arnold's own DLLs from the MtoA install, put on PATH.
if(TARGET Arnold::Arnold AND (MTOA_LOCATION OR GROOMLAB_ARNOLD_SDK))
    add_executable(groomlab_arnold_tests hosts/arnold/test_arnold_curves.cpp)
    target_link_libraries(groomlab_arnold_tests PRIVATE groomlab::core Arnold::Arnold GTest::gtest_main)
    target_include_directories(groomlab_arnold_tests PRIVATE "${PROJECT_SOURCE_DIR}/hosts/arnold/include")
    if(GROOMLAB_TBB_RUNTIME)
        add_custom_command(TARGET groomlab_arnold_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:groomlab_arnold_tests> $<TARGET_FILE_DIR:groomlab_arnold_tests>
            COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME arnold.curves COMMAND groomlab_arnold_tests)
    if(WIN32)
        set_tests_properties(arnold.curves PROPERTIES LABELS "arnold" TIMEOUT 600
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${MTOA_LOCATION}/bin")
    else()
        # libai.so through the executable's build RUNPATH (it links it by path).
        set_tests_properties(arnold.curves PROPERTIES LABELS "arnold" TIMEOUT 600)
    endif()
endif()

# Arnold translator test: exports a 100k-strand groom through MtoA to .ass.
if(TARGET groomLabTranslator)
    add_test(NAME maya.arnold
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_arnold.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE_DIR:groomLabTranslator>"
                "${CMAKE_CURRENT_BINARY_DIR}/groomlab_test.ass"
                "$<$<TARGET_EXISTS:groomlab_procedural>:$<TARGET_FILE_DIR:groomlab_procedural>>")
    # kick stalls when several Maya sessions start alongside it (ctest -j): run it alone.
    set_tests_properties(maya.arnold PROPERTIES RUN_SERIAL TRUE LABELS "arnold;maya")
endif()

# Studio lighting (groomlab.studio, needs MtoA): every preset's rig, its scaling, framing, undo and
# what remove_studio restores (quick); and a small kick render of the portrait preset (arnold, alone
# as maya.arnold is).
if(TARGET groomLabTranslator)
    foreach(_studio IN ITEMS rig render)
        set(_studio_name maya.studio)
        set(_studio_args "")
        if(_studio STREQUAL "render")
            set(_studio_name maya.studio_render)
            set(_studio_args "--render")
        endif()
        add_test(NAME ${_studio_name}
            COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_studio.py"
                    "$<TARGET_FILE:GroomLab>"
                    "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                    "$<TARGET_FILE_DIR:groomLabTranslator>"
                    "${CMAKE_CURRENT_BINARY_DIR}/studio"
                    ${_studio_args})
    endforeach()
    set_tests_properties(maya.studio_render PROPERTIES RUN_SERIAL TRUE LABELS "arnold;maya")
endif()

# The RenderMan procedural in prman (hosts/renderman), with RenderMan Pro Server only: counts against
# groomlab-cli, coverage and primvars rendered, motion keys, the example scene, and the older API's
# data string (RenderMan for Maya's RenderManProcedural node). Seconds, but prman's
# license is one per machine: alone.
if(TARGET groomlab_renderman AND TARGET groomlab-cli AND RenderMan_PRMAN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME renderman.procedural
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/renderman/test_renderman.py"
                    "${RenderMan_PRMAN}" "$<TARGET_FILE_DIR:groomlab_renderman>" "$<TARGET_FILE:groomlab-cli>"
                    "${CMAKE_CURRENT_BINARY_DIR}/renderman")
        set_tests_properties(renderman.procedural PROPERTIES LABELS "renderman" RUN_SERIAL TRUE TIMEOUT 600)
    endif()
endif()

# The V-Ray scene export (vrscene_writer.h) in V-Ray Standalone, when it is installed: the head groom
# written by groomlab-cli and rendered small (vray_compare.py), its coverage, colors and motion blur;
# with MtoA's kick and GroomLab's Arnold procedural (the Maya build) a comparison with Arnold too.
# V-Ray's license is checked out per render: alone.
find_program(GROOMLAB_VRAY_STANDALONE vray
    PATHS "$ENV{ProgramFiles}/Chaos/V-Ray/Standalone for x64/bin" "$ENV{VRAY_PATH}" NO_CACHE)
if(GROOMLAB_VRAY_STANDALONE AND TARGET groomlab-cli)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(_vray_arnold "")
        if(TARGET groomlab_procedural AND MTOA_LOCATION AND EXISTS "${MTOA_LOCATION}/bin/kick${CMAKE_EXECUTABLE_SUFFIX}")
            set(_vray_arnold --kick "${MTOA_LOCATION}/bin/kick${CMAKE_EXECUTABLE_SUFFIX}"
                             --procedural "$<TARGET_FILE_DIR:groomlab_procedural>")
        endif()
        add_test(NAME vray.render
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/vray/test_vray.py"
                    "${GROOMLAB_VRAY_STANDALONE}" "$<TARGET_FILE:groomlab-cli>" "${CMAKE_CURRENT_BINARY_DIR}/vray"
                    ${_vray_arnold})
        set_tests_properties(vray.render PROPERTIES LABELS "vray" RUN_SERIAL TRUE TIMEOUT 900)
    endif()
endif()

# V-Ray Standalone scenes and renders from Maya (groomlab.vray_scene, vray_render, vray_hook): the
# scene text, vray.exe's command line and progress, and the V-Ray for Maya hook's frames, script and
# plugin edits against a stand-in vray.utils, in plain Python (quick); the studio's scene written
# from Maya and, with vray.exe, rendered small (maya.vray, alone: V-Ray's license is per render).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME vray.scene
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/vray/test_vray_scene.py"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
    set_tests_properties(vray.scene PROPERTIES LABELS "quick;vray")
endif()
if(TARGET groomLabTranslator)
    add_test(NAME maya.vray
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_vray_render.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE_DIR:groomLabTranslator>"
                "${CMAKE_CURRENT_BINARY_DIR}/vray_maya"
                "${GROOMLAB_VRAY_STANDALONE}")
    set_tests_properties(maya.vray PROPERTIES RUN_SERIAL TRUE TIMEOUT 900)
endif()

# RenderMan scenes and renders from Maya (groomlab.prman_scene, prman_render, render_common): the RIB
# text, prman's command line, progress and finding it, in plain Python (quick); with prman, the
# conventions rendered (a floor under each kind of light); the studio's scene written from Maya and,
# with prman and the procedural, Nika's groom rendered small (maya.prman). prman's license is one per
# machine: the renders alone.
if(Python3_Interpreter_FOUND)
    add_test(NAME renderman.scene
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/renderman/test_prman_scene.py"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts")
    set_tests_properties(renderman.scene PROPERTIES LABELS "quick;renderman")
    if(RenderMan_PRMAN)
        add_test(NAME renderman.scene_render
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/hosts/renderman/test_prman_scene.py"
                    "${PROJECT_SOURCE_DIR}/hosts/maya/scripts" "${RenderMan_PRMAN}")
        set_tests_properties(renderman.scene_render PROPERTIES LABELS "renderman" RUN_SERIAL TRUE TIMEOUT 600)
    endif()
endif()
if(TARGET groomLabTranslator)
    add_test(NAME maya.prman
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/test_prman_render.py"
                "$<TARGET_FILE:GroomLab>"
                "${PROJECT_SOURCE_DIR}/hosts/maya/scripts"
                "$<TARGET_FILE_DIR:groomLabTranslator>"
                "${CMAKE_CURRENT_BINARY_DIR}/prman_maya"
                "$<$<TARGET_EXISTS:groomlab_renderman>:$<TARGET_FILE_DIR:groomlab_renderman>>"
                "${RenderMan_PRMAN}")
    set_tests_properties(maya.prman PROPERTIES RUN_SERIAL TRUE TIMEOUT 900)
endif()

# GroomLab's Maya windows on small screens and with large text (125-150%), headless: mayapy's
# PySide2, Qt offscreen and a stand-in Maya; no window on the desktop.
if(GROOMLAB_BUILD_MAYA AND TARGET groomlab_core_py)
    add_test(NAME maya.ui_scaling
        COMMAND "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}"
                "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/ui_scaling_check.py"
                --core "$<TARGET_FILE_DIR:groomlab_core_py>" --dll-dir "${MAYA_LOCATION}/bin")
    set_tests_properties(maya.ui_scaling PROPERTIES LABELS "quick;maya" TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GROOMLAB_NO_UPDATES=1")
endif()

# Every mayapy test gets a prefs folder of its own: standalone Maya also reads
# the user's plugin auto-load list, and an installed GroomLab module would
# then be loaded instead of the build's plugin. One folder per test, so the
# suites running side by side (ctest -j) never write the same prefs files.
# The plugin's Python imports the build's groomlab_core module (as the
# installed module's PYTHONPATH has it). COST starts the long suites first.
set(GROOMLAB_MAYA_PYTHONPATH "$<$<TARGET_EXISTS:groomlab_core_py>:$<TARGET_FILE_DIR:groomlab_core_py>>")
foreach(_maya_test maya.plugin maya.graph_editor maya.pipeline maya.groom_io maya.grooming maya.strand_mesh maya.subdivision maya.hardening maya.updates maya.arnold maya.studio maya.studio_render maya.vray maya.prman maya.xgen_import maya.yeti_import)
    if(TEST ${_maya_test})
        set(_app_dir "${CMAKE_CURRENT_BINARY_DIR}/maya_app_dirs/${_maya_test}")
        file(MAKE_DIRECTORY "${_app_dir}")
        set_tests_properties(${_maya_test} PROPERTIES COST 1000 ENVIRONMENT
            "MAYA_APP_DIR=${_app_dir};PYTHONPATH=${GROOMLAB_MAYA_PYTHONPATH}")
        if(NOT _maya_test STREQUAL "maya.arnold" AND NOT _maya_test STREQUAL "maya.studio_render"
           AND NOT _maya_test STREQUAL "maya.vray" AND NOT _maya_test STREQUAL "maya.prman")
            set_tests_properties(${_maya_test} PROPERTIES LABELS "quick;maya")
        endif()
        if(_maya_test STREQUAL "maya.vray")
            set_tests_properties(${_maya_test} PROPERTIES LABELS "vray;maya")
        endif()
        if(_maya_test STREQUAL "maya.prman")
            set_tests_properties(${_maya_test} PROPERTIES LABELS "renderman;maya")
        endif()
    endif()
endforeach()

# The GUI session (one Maya window, a few minutes): every GUI suite (tests/hosts/maya/gui_*.py: the
# interactive brush and selection tools, reference picking, the viewport budget, the Graph
# Editor and Groom Tools widgets, quitting with them open) in turn, through gui_runner.py. Opt in
# with -DGROOMLAB_GUI_TESTS=ON.
option(GROOMLAB_GUI_TESTS "Add tests that launch the Maya UI" OFF)
if(GROOMLAB_GUI_TESTS AND GROOMLAB_BUILD_MAYA AND TARGET GroomLab)
    # Maya's secure plugin loading asks before loading from an untrusted folder, and asks again
    # when the trusted list changes while it runs: dialogs nobody is there to click. Before the
    # sessions start, the build's plugin folder goes into the tests' own prefs file, which Maya
    # reads at startup without a question.
    add_test(NAME maya.gui_trust
        COMMAND "${CMAKE_COMMAND}" "-DAPP_DIR=${CMAKE_CURRENT_BINARY_DIR}/maya_app_dir"
                "-DDIR=$<TARGET_FILE_DIR:GroomLab>" -P "${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/trust_plugin_folder.cmake")
    set_tests_properties(maya.gui_trust PROPERTIES FIXTURES_SETUP maya_gui_trust LABELS "gui;maya")
    # GROOMLAB_GUI_SUITES (brush,viewport,...) in ctest's environment runs only those suites.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gui.mel"
        "python(\"exec(open(r'${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya/gui_runner.py').read())\");\n")
    add_test(NAME maya.gui
        COMMAND "${MAYA_LOCATION}/bin/maya${CMAKE_EXECUTABLE_SUFFIX}" -script "${CMAKE_CURRENT_BINARY_DIR}/gui.mel")
    set_tests_properties(maya.gui PROPERTIES
        TIMEOUT 900
        RUN_SERIAL TRUE
        LABELS "gui;maya"
        FIXTURES_REQUIRED maya_gui_trust
        # A prefs folder of its own: the user's prefs could auto-load an installed
        # GroomLab module, and the build's plugin would then be skipped.
        ENVIRONMENT "MAYA_DEBUG_ENABLE_CRASH_REPORTING=0;MAYA_APP_DIR=${CMAKE_CURRENT_BINARY_DIR}/maya_app_dir;PYTHONPATH=${GROOMLAB_MAYA_PYTHONPATH};GROOMLAB_PLUGIN=$<TARGET_FILE:GroomLab>;GROOMLAB_SCRIPTS=${PROJECT_SOURCE_DIR}/hosts/maya/scripts;GROOMLAB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya;GROOMLAB_GUI_LOG=${CMAKE_CURRENT_BINARY_DIR}/gui.log")
    # The drawing suites again on DirectX 11: fresh prefs give OpenGL, but many artists run
    # Viewport 2.0 on DirectX 11, where buffers behave differently (GPU-only vertex buffers
    # cannot be written there). MAYA_VP2_DEVICE_OVERRIDE picks the device without touching prefs.
    # The quit suite too: Maya quits with the panels open on that device as well; cached
    # playback, whose frames the viewport draws from the cache; and Hardware 2.0 renders
    # (gui_render.py), which draw through Viewport 2.0.
    add_test(NAME maya.gui_dx11
        COMMAND "${MAYA_LOCATION}/bin/maya${CMAKE_EXECUTABLE_SUFFIX}" -script "${CMAKE_CURRENT_BINARY_DIR}/gui.mel")
    set_tests_properties(maya.gui_dx11 PROPERTIES
        TIMEOUT 900
        RUN_SERIAL TRUE
        LABELS "gui;maya"
        FIXTURES_REQUIRED maya_gui_trust
        ENVIRONMENT "MAYA_DEBUG_ENABLE_CRASH_REPORTING=0;MAYA_VP2_DEVICE_OVERRIDE=VirtualDeviceDx11;GROOMLAB_GUI_SUITES=viewport,brush,cached_playback,render,quit;MAYA_APP_DIR=${CMAKE_CURRENT_BINARY_DIR}/maya_app_dir;PYTHONPATH=${GROOMLAB_MAYA_PYTHONPATH};GROOMLAB_PLUGIN=$<TARGET_FILE:GroomLab>;GROOMLAB_SCRIPTS=${PROJECT_SOURCE_DIR}/hosts/maya/scripts;GROOMLAB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya;GROOMLAB_GUI_LOG=${CMAKE_CURRENT_BINARY_DIR}/gui_dx11.log")
    # The presets' contact sheets (gui_presets.py): every shipped preset applied as the Presets menu
    # applies it and drawn by Viewport 2.0, on OpenGL and on DirectX 11, into
    # GROOMLAB_PRESET_SHEET_DIR (default: <temp>/groomlab_preset_sheet). A look-over tool (label
    # "sheet", in no other label): ctest --preset ninja-maya2022 -L sheet.
    foreach(_sheet IN ITEMS gl dx11)
        set(_sheet_env "")
        set(_sheet_name maya.preset_sheet)
        if(_sheet STREQUAL "dx11")
            set(_sheet_env "MAYA_VP2_DEVICE_OVERRIDE=VirtualDeviceDx11;")
            set(_sheet_name maya.preset_sheet_dx11)
        endif()
        add_test(NAME ${_sheet_name}
            COMMAND "${MAYA_LOCATION}/bin/maya${CMAKE_EXECUTABLE_SUFFIX}" -script "${CMAKE_CURRENT_BINARY_DIR}/gui.mel")
        set_tests_properties(${_sheet_name} PROPERTIES
            TIMEOUT 3600
            RUN_SERIAL TRUE
            LABELS "sheet"
            FIXTURES_REQUIRED maya_gui_trust
            ENVIRONMENT "MAYA_DEBUG_ENABLE_CRASH_REPORTING=0;${_sheet_env}GROOMLAB_GUI_SUITES=presets;GROOMLAB_GUI_SUITE_TIMEOUT=3500;MAYA_APP_DIR=${CMAKE_CURRENT_BINARY_DIR}/maya_app_dir;PYTHONPATH=${GROOMLAB_MAYA_PYTHONPATH};GROOMLAB_PLUGIN=$<TARGET_FILE:GroomLab>;GROOMLAB_SCRIPTS=${PROJECT_SOURCE_DIR}/hosts/maya/scripts;GROOMLAB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya;GROOMLAB_GUI_LOG=${CMAKE_CURRENT_BINARY_DIR}/preset_sheet_${_sheet}.log")
    endforeach()
    # The viewport benchmark (gui_bench.py: numbers, not checks), on OpenGL and on DirectX 11:
    # label "bench" alone (ctest --preset ninja-maya2022 -L bench). GROOMLAB_BENCH_COUNTS and
    # GROOMLAB_BENCH_OUT pass through from ctest's environment.
    foreach(_bench IN ITEMS gl dx11)
        set(_bench_env "")
        set(_bench_name maya.viewport_bench)
        if(_bench STREQUAL "dx11")
            set(_bench_env "MAYA_VP2_DEVICE_OVERRIDE=VirtualDeviceDx11;")
            set(_bench_name maya.viewport_bench_dx11)
        endif()
        add_test(NAME ${_bench_name}
            COMMAND "${MAYA_LOCATION}/bin/maya${CMAKE_EXECUTABLE_SUFFIX}" -script "${CMAKE_CURRENT_BINARY_DIR}/gui.mel")
        set_tests_properties(${_bench_name} PROPERTIES
            TIMEOUT 3600
            RUN_SERIAL TRUE
            LABELS "bench"
            FIXTURES_REQUIRED maya_gui_trust
            ENVIRONMENT "MAYA_DEBUG_ENABLE_CRASH_REPORTING=0;${_bench_env}GROOMLAB_GUI_SUITES=bench;GROOMLAB_GUI_SUITE_TIMEOUT=3500;MAYA_APP_DIR=${CMAKE_CURRENT_BINARY_DIR}/maya_app_dir;PYTHONPATH=${GROOMLAB_MAYA_PYTHONPATH};GROOMLAB_PLUGIN=$<TARGET_FILE:GroomLab>;GROOMLAB_SCRIPTS=${PROJECT_SOURCE_DIR}/hosts/maya/scripts;GROOMLAB_TESTS=${CMAKE_CURRENT_SOURCE_DIR}/hosts/maya;GROOMLAB_GUI_LOG=${CMAKE_CURRENT_BINARY_DIR}/viewport_bench_${_bench}.log")
    endforeach()
endif()
