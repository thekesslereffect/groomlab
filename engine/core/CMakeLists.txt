# SPDX-License-Identifier: Apache-2.0
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/groomlab/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/groomlab/version.h"
    @ONLY)

groomlab_fetch_json()

# The core's code, compiled once: groomlab_core is it with the TBB backend (parallel.h), and the C
# API's library links groomlab_core_private, the same objects with its own threads instead
# (engine/capi: a host's TBB never meets the library's).
add_library(groomlab_core_objects OBJECT
    src/brush.cpp
    src/brush_alpha.cpp
    src/brush_catalog.cpp
    src/camera_solve.cpp
    src/cards.cpp
    src/card_separation.cpp
    src/color_patterns.cpp
    src/card_atlas.cpp
    src/card_bake.cpp
    src/card_feathers.cpp
    src/card_files.cpp
    src/card_maps.cpp
    src/card_presets.cpp
    src/hair_color.cpp
    src/renderman_look.cpp
    src/curve.cpp
    src/distance_field.cpp
    src/display.cpp
    src/display_overlays.cpp
    src/strand_lod.cpp
    src/expression.cpp
    src/export_view.cpp
    src/feathers.cpp
    src/gltf_writer.cpp
    src/field.cpp
    src/frame_time.cpp
    src/frames.cpp
    src/graph.cpp
    src/groom.cpp
    src/groom_session.cpp
    src/async_evaluation.cpp
    src/attributes.cpp
    src/groom_file.cpp
    src/graph_io.cpp
    src/graph_edit.cpp
    src/graph_pins.cpp
    src/graph_compound.cpp
    src/guide_hierarchy.cpp
    src/guide_selection.cpp
    src/length_field.cpp
    src/length_key_tool.cpp
    src/guide_set.cpp
    src/tube.cpp
    src/instances.cpp
    src/node_catalog.cpp
    src/node_library.cpp
    src/noise.cpp
    src/look/look_run.cpp
    src/look/look_engine.cpp
    src/look/look_engine_cpu.cpp
    src/look/look_cpu.cpp
    src/look/look_stage_params.cpp
    src/look/generated/look_kernels_cpu.cpp
    src/node_common.cpp
    src/parts.cpp
    src/transfer.cpp
    src/transfer_markers_edit.cpp
    src/paths.cpp
    src/presets.cpp
    src/shortcuts.cpp
    src/png.cpp
    src/ptex.cpp
    src/point_index.cpp
    src/ramp.cpp
    src/reference_edit.cpp
    src/reference_guides.cpp
    src/reference_look.cpp
    src/reference_orientation.cpp
    src/render_overrides.cpp
    src/scalp_history.cpp
    src/scalp_mesh.cpp
    src/scalp_pieces.cpp
    src/subdivision.cpp
    src/scalp_maps.cpp
    src/scalp_paint.cpp
    src/scale_maps.cpp
    src/scales.cpp
    src/script.cpp
    src/simulation.cpp
    src/view_projection.cpp
    src/schema.cpp
    src/semver.cpp
    src/lod.cpp
    src/correctives.cpp
    src/sculpt.cpp
    src/flow.cpp
    src/follow_scalp.cpp
    src/strand_geometry.cpp
    src/preview_core.cpp
    src/process_caches.cpp
    src/strand_set.cpp
    src/surface_paths.cpp
    src/stroke.cpp
    src/stroke_tools.cpp
    src/scalp_mirror.cpp
    src/texture.cpp
    src/usd_writer.cpp
    src/vrscene_writer.cpp
    src/operators/compound.cpp
    src/operators/script_op.cpp
    src/operators/deform_ops.cpp
    src/operators/interpolate.cpp
    src/operators/braid_op.cpp
    src/operators/extra_ops.cpp
    src/operators/feather_ops.cpp
    src/operators/plume_op.cpp
    src/operators/cinematic_ops.cpp
    src/operators/color_ops.cpp
    src/operators/field_ops.cpp
    src/operators/incremental.cpp
    src/operators/instance_op.cpp
    src/operators/look_ops.cpp
    src/operators/curl_op.cpp
    src/operators/mesh_cut_op.cpp
    src/operators/mirror_op.cpp
    src/operators/motion_ops.cpp
    src/operators/neighbor_ops.cpp
    src/operators/registry.cpp
    src/operators/reference_op.cpp
    src/operators/render_data_op.cpp
    src/operators/scatter.cpp
    src/operators/sculpt_op.cpp
    src/operators/self_collide_op.cpp
    src/operators/shape_ops.cpp
    src/operators/simulate_op.cpp
    src/operators/sources.cpp
    src/operators/static_op.cpp
    src/operators/strand_mesh_op.cpp
    src/operators/transform_ops.cpp
    src/operators/wet_op.cpp
    src/operators/group_op.cpp
    src/operators/hair_grid.cpp
    src/operators/subdivide_op.cpp
)
# What every user of the core's objects needs: its headers.
add_library(groomlab_core_headers INTERFACE)
target_include_directories(groomlab_core_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(groomlab_core_headers INTERFACE "GROOMLAB_PORTABLE_MATH=$<BOOL:${GROOMLAB_PORTABLE_MATH}>")

target_link_libraries(groomlab_core_objects PUBLIC groomlab_core_headers)
# stb_image (public domain, vendored): texture maps.
target_include_directories(groomlab_core_objects SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
# The look kernels' C++ (slangc's, src/look/generated): Slang's prelude (third_party/slang), its own
# style (warnings off for it), the support header beside it. MSVC inlines it as far as it goes
# (/Ob3: its many small helpers, none marked inline, calls under /Ob2 in the kernels' loops; about
# 20% off Twist and Wind), no float result changed (/fp:precise).
target_include_directories(groomlab_core_objects SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/slang/include")
target_include_directories(groomlab_core_objects PRIVATE src/look)
set(_kernels_cpp "${CMAKE_CURRENT_SOURCE_DIR}/src/look/generated/look_kernels_cpu.cpp")
set_source_files_properties("${_kernels_cpp}" PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0;/bigobj;/Ob3,-w>")
# With MSVC, the kernels built by clang-cl where Visual Studio has it (its LLVM tools; else MSVC's):
# about twice as fast for the heavier ones (Interpolate, Wind, SelfCollide, Collide), the same
# floats (x64's SSE2, nothing contracted into fused multiply-adds, no strict aliasing: the prelude
# reads a value through another type's pointer). Its object joins the core's libraries
# (GROOMLAB_KERNEL_OBJECTS).
set(GROOMLAB_KERNEL_OBJECTS "")
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    option(GROOMLAB_KERNELS_CLANG "Build the look kernels' C++ with clang-cl (MSVC builds)" ON)
    if(GROOMLAB_KERNELS_CLANG)
        get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(GROOMLAB_CLANG_CL clang-cl HINTS "${_cl_dir}/../../../../../Llvm/x64/bin" "$ENV{VCINSTALLDIR}Tools/Llvm/x64/bin"
                     NO_DEFAULT_PATH)
    endif()
    if(GROOMLAB_KERNELS_CLANG AND GROOMLAB_CLANG_CL)
        message(STATUS "GroomLab: look kernels built by ${GROOMLAB_CLANG_CL}")
        set(_kernels_obj "${CMAKE_CURRENT_BINARY_DIR}/look_kernels_cpu_clang_$<CONFIG>.obj")
        set(_kernels_dep "${CMAKE_CURRENT_BINARY_DIR}/look_kernels_cpu_clang_$<CONFIG>.d")
        set(_includes "$<TARGET_PROPERTY:groomlab_core_objects,INCLUDE_DIRECTORIES>")
        set(_defines "$<TARGET_PROPERTY:groomlab_core_objects,COMPILE_DEFINITIONS>")
        # engine/gpu makes the C++ again from look.slang (its stamp: the file itself is written aside,
        # so a build would see it unchanged and compile the stale one): built after it, again with it.
        set(_kernels_stamp "${CMAKE_BINARY_DIR}/engine/gpu/generated/look_kernels_cpu.stamp")
        add_custom_command(
            OUTPUT "${_kernels_obj}"
            COMMAND "${GROOMLAB_CLANG_CL}" /nologo /c /TP -std:c++17 /EHsc /utf-8 -w -Z7
                    "$<IF:$<CONFIG:Debug>,/Od;/MDd;/D_DEBUG,/O2;/MD;/DNDEBUG>"
                    /clang:-fno-strict-aliasing /clang:-ffp-contract=off
                    "$<$<BOOL:${_includes}>:/I$<JOIN:${_includes},;/I>>"
                    "$<$<BOOL:${_defines}>:/D$<JOIN:${_defines},;/D>>"
                    /clang:-MD "/clang:-MF${_kernels_dep}" "/Fo${_kernels_obj}" "${_kernels_cpp}"
            DEPENDS "${_kernels_cpp}" "${_kernels_stamp}"
            DEPFILE "${_kernels_dep}"
            COMMENT "Building the look kernels with clang-cl"
            COMMAND_EXPAND_LISTS
            VERBATIM)
        set_source_files_properties("${_kernels_cpp}" PROPERTIES HEADER_FILE_ONLY ON)
        set_source_files_properties("${_kernels_obj}" PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
        set(GROOMLAB_KERNEL_OBJECTS "${_kernels_obj}")
    endif()
endif()
target_link_libraries(groomlab_core_objects PRIVATE nlohmann_json::nlohmann_json groomlab::warnings)
groomlab_precompile(groomlab_core_objects <nlohmann/json.hpp>)

if(GROOMLAB_HAS_TBB)
    add_library(groomlab_core STATIC $<TARGET_OBJECTS:groomlab_core_objects> ${GROOMLAB_KERNEL_OBJECTS} src/parallel_tbb.cpp)
    target_link_libraries(groomlab_core PUBLIC groomlab::tbb)
    target_compile_definitions(groomlab_core PUBLIC GROOMLAB_HAS_TBB=1)
else()
    add_library(groomlab_core STATIC $<TARGET_OBJECTS:groomlab_core_objects> ${GROOMLAB_KERNEL_OBJECTS} src/parallel_serial.cpp)
    target_compile_definitions(groomlab_core PUBLIC GROOMLAB_HAS_TBB=0)
endif()
add_library(groomlab::core ALIAS groomlab_core)
target_link_libraries(groomlab_core PUBLIC groomlab_core_headers PRIVATE groomlab::warnings)

if(GROOMLAB_BUILD_CAPI)
    find_package(Threads REQUIRED)
    add_library(groomlab_core_private STATIC $<TARGET_OBJECTS:groomlab_core_objects> ${GROOMLAB_KERNEL_OBJECTS} src/parallel_pool.cpp)
    target_link_libraries(groomlab_core_private PUBLIC groomlab_core_headers Threads::Threads PRIVATE groomlab::warnings)
endif()
