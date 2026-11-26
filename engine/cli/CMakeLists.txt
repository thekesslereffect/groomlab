# SPDX-License-Identifier: Apache-2.0
add_executable(groomlab-cli main.cpp)
target_link_libraries(groomlab-cli PRIVATE groomlab::core groomlab::warnings)
if(TARGET groomlab_alembic)
    # The exports (eval, the MCP server's export) and the MCP server itself (mcp_server.h: an agent
    # drives grooms through it), which evaluate grooms through GroomScene (engine/io).
    target_sources(groomlab-cli PRIVATE
        cli_export.cpp
        cli_export.h
        mcp_groom.cpp
        mcp_groom.h
        mcp_server.cpp
        mcp_server.h
        mesh_files.cpp
        mesh_files.h)
    # The tools themselves are the editor's (agent_tools.h); the server holds the protocol and the
    # grooms it opens in memory (mcp_groom.h).
    target_link_libraries(groomlab-cli PRIVATE groomlab::alembic groomlab::editor nlohmann_json::nlohmann_json)
    target_compile_definitions(groomlab-cli PRIVATE GROOMLAB_HAS_ALEMBIC=1)
    # A build tree's server finds GroomLab's presets, nodes, sample meshes and docs in the sources;
    # an installed one beside it (GroomLab/bin/../presets and so on).
    groomlab_path_literal(_cli_presets "${PROJECT_SOURCE_DIR}/engine/assets/presets")
    groomlab_path_literal(_cli_nodes "${PROJECT_SOURCE_DIR}/engine/assets/nodes")
    groomlab_path_literal(_cli_meshes "${PROJECT_SOURCE_DIR}/engine/assets/meshes")
    groomlab_path_literal(_cli_docs "${PROJECT_SOURCE_DIR}/web/content/docs")
    target_compile_definitions(groomlab-cli PRIVATE "GROOMLAB_SHIPPED_PRESETS=${_cli_presets}"
        "GROOMLAB_SHIPPED_NODES=${_cli_nodes}" "GROOMLAB_SHIPPED_MESHES=${_cli_meshes}" "GROOMLAB_DOCS_DIR=${_cli_docs}")
else()
    target_compile_definitions(groomlab-cli PRIVATE GROOMLAB_HAS_ALEMBIC=0)
endif()

if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    add_custom_command(TARGET groomlab-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:groomlab-cli> $<TARGET_FILE_DIR:groomlab-cli>
        COMMAND_EXPAND_LISTS)
endif()

# Shipped in the Maya module (GroomLab/bin) with its runtime DLLs, for farms.
install(TARGETS groomlab-cli RUNTIME DESTINATION "GroomLab/bin" COMPONENT GroomLab)
groomlab_install_legal("GroomLab" GroomLab)  # also without the Maya plugin (a Linux farm build)
groomlab_install_runtime(groomlab-cli "GroomLab/bin" GroomLab)
if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    install(FILES $<TARGET_RUNTIME_DLLS:groomlab-cli> DESTINATION "GroomLab/bin" COMPONENT GroomLab)
endif()
# The docs' pages beside it, for the MCP server's get_docs (GroomLab/docs/pages); and without the
# Maya plugin (a Linux build), the presets, nodes and sample meshes the Maya module would bring.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/web/content/docs/" DESTINATION "GroomLab/docs/pages" COMPONENT GroomLab
        FILES_MATCHING PATTERN "*.mdx")
if(NOT GROOMLAB_BUILD_MAYA)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/assets/presets/" DESTINATION "GroomLab/presets" COMPONENT GroomLab)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/assets/nodes/" DESTINATION "GroomLab/nodes" COMPONENT GroomLab)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/assets/meshes/" DESTINATION "GroomLab/meshes" COMPONENT GroomLab
            PATTERN "*_rig.*" EXCLUDE)  # the hosts' rigs: the app grooms buddy.obj
endif()
