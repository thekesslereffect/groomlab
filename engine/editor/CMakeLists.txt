# SPDX-License-Identifier: Apache-2.0
# The groom editor: what every host's editing of a groom shares (its undo steps and history; the
# user's actions as they move in), on the core. The app links it; the Blender helper and Maya's
# bindings take it as their actions move in.
add_library(groomlab_editor STATIC
    src/graph_edits.cpp
    src/groom_edit.cpp
)
add_library(groomlab::editor ALIAS groomlab_editor)

target_include_directories(groomlab_editor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(groomlab_editor
    PUBLIC groomlab::core nlohmann_json::nlohmann_json
    PRIVATE groomlab::warnings)
