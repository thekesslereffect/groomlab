# SPDX-License-Identifier: Apache-2.0
# groomlab-helper: GroomLab's core in a process of its own, which the Blender add-on (GPL, next to
# it in GroomLabBlender/) spawns and talks to over stdin/stdout and shared memory (docs/BLENDER.md).
# Apache-2.0 like the rest of GroomLab (the add-on is GPL-3.0-or-later): it is never loaded into
# Blender's process.

add_library(groomlab_helper_objs OBJECT src/helper.cpp src/tools.cpp src/authoring.cpp src/pipeline.cpp src/reference.cpp src/shared_memory.cpp)
target_link_libraries(groomlab_helper_objs PUBLIC groomlab::core groomlab::warnings nlohmann_json::nlohmann_json)
if(TARGET groomlab_alembic)
    target_link_libraries(groomlab_helper_objs PUBLIC groomlab::alembic)
    target_compile_definitions(groomlab_helper_objs PUBLIC GROOMLAB_HAS_ALEMBIC=1)
else()
    target_compile_definitions(groomlab_helper_objs PUBLIC GROOMLAB_HAS_ALEMBIC=0)
endif()
# A build tree's helper finds the shipped presets in the sources; an installed one beside it (bin/presets).
groomlab_path_literal(_helper_presets "${PROJECT_SOURCE_DIR}/hosts/maya/presets")
target_compile_definitions(groomlab_helper_objs PRIVATE "GROOMLAB_SHIPPED_PRESETS=${_helper_presets}")
# The compound nodes shipped with every host, the same way (bin/nodes when installed).
groomlab_path_literal(_helper_nodes "${PROJECT_SOURCE_DIR}/hosts/maya/nodes")
target_compile_definitions(groomlab_helper_objs PRIVATE "GROOMLAB_SHIPPED_NODES=${_helper_nodes}")
# The brush alphas shipped with every host, the same way (bin/alphas when installed).
groomlab_path_literal(_helper_alphas "${PROJECT_SOURCE_DIR}/hosts/maya/alphas")
target_compile_definitions(groomlab_helper_objs PRIVATE "GROOMLAB_SHIPPED_ALPHAS=${_helper_alphas}")
# The sample meshes (Nika, Manny, the wolf, Buddy's rig: CC BY 4.0) the add-on adds, the same way (bin/meshes when installed).
groomlab_path_literal(_helper_meshes "${PROJECT_SOURCE_DIR}/hosts/maya/meshes")
target_compile_definitions(groomlab_helper_objs PRIVATE "GROOMLAB_SHIPPED_MESHES=${_helper_meshes}")

# The shipped helper and the tests' build of it (test hooks) share everything but main's object.
function(groomlab_helper_executable name)
    add_executable(${name} src/main.cpp)
    target_link_libraries(${name} PRIVATE groomlab_helper_objs)
    if(WIN32 AND GROOMLAB_TBB_RUNTIME)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${name}> $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

groomlab_helper_executable(groomlab-helper)

# The same helper with test hooks: the tests' only, never installed.
if(GROOMLAB_BUILD_TESTS)
    groomlab_helper_executable(groomlab-helper-testhooks)
    # An operator at version 2 (TestCApiLift), so the add-on's Graph Editor test has an outdated
    # node to Upgrade (no built-in operator has a version 2 yet).
    target_sources(groomlab-helper-testhooks PRIVATE "${PROJECT_SOURCE_DIR}/tests/engine/capi/capi_versioned_op.cpp")
endif()

# The Blender package's helper half: GroomLabBlender/bin with the helper, its runtime DLLs, the
# shipped presets, nodes and sample meshes, the helper's license (Apache-2.0: LICENSE, NOTICE, LICENSE-HELPER.txt) and the
# third-party notices. The add-on (GPL) goes beside bin/ in GroomLabBlender/.
set(GROOMLAB_BLENDER_BIN "GroomLabBlender/bin")
install(TARGETS groomlab-helper RUNTIME DESTINATION "${GROOMLAB_BLENDER_BIN}" COMPONENT Blender)
groomlab_install_runtime(groomlab-helper "${GROOMLAB_BLENDER_BIN}" Blender)
if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    install(FILES $<TARGET_RUNTIME_DLLS:groomlab-helper> DESTINATION "${GROOMLAB_BLENDER_BIN}" COMPONENT Blender)
endif()
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/presets/" DESTINATION "${GROOMLAB_BLENDER_BIN}/presets" COMPONENT Blender)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/nodes/" DESTINATION "${GROOMLAB_BLENDER_BIN}/nodes" COMPONENT Blender)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/alphas/" DESTINATION "${GROOMLAB_BLENDER_BIN}/alphas" COMPONENT Blender)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/meshes/" DESTINATION "${GROOMLAB_BLENDER_BIN}/meshes" COMPONENT Blender
        PATTERN "*.ma" EXCLUDE)  # Buddy's Maya rig (Blender's is buddy_rig.fbx)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE-HELPER.txt" DESTINATION "${GROOMLAB_BLENDER_BIN}" COMPONENT Blender)
groomlab_install_legal("${GROOMLAB_BLENDER_BIN}" Blender)
