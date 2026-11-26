# SPDX-License-Identifier: Apache-2.0
# The groom editor: what every host's editing of a groom shares (its undo steps and history; the
# user's actions as they move in), on the core. The app links it; the Blender helper and Maya's
# bindings take it as their actions move in.
add_library(groomlab_editor STATIC
    src/agent_canvas.cpp
    src/agent_expressions.cpp
    src/agent_grooming.cpp
    src/agent_photos.cpp
    src/agent_preview.cpp
    src/agent_target.cpp
    src/agent_tools.cpp
    src/game_exports.cpp
    src/graph_edits.cpp
    src/groom_edit.cpp
    src/guide_edits.cpp
    src/map_edits.cpp
    src/tool_overlay.cpp
    src/tool_select.cpp
    src/tool_settings.cpp
)
add_library(groomlab::editor ALIAS groomlab_editor)

target_include_directories(groomlab_editor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(groomlab_editor
    PUBLIC groomlab::core nlohmann_json::nlohmann_json
    PRIVATE groomlab::warnings)
