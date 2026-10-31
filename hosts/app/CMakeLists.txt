# SPDX-License-Identifier: Apache-2.0
# The standalone GroomLab app (GROOMLAB_BUILD_APP): a lookdev viewer over the core without Maya.
# Qt 6 Widgets for the windows and panels, Qt's RHI (QRhi: Direct3D 11/12, Vulkan, Metal, OpenGL)
# for the viewport, so one renderer runs on every platform. Qt is linked dynamically (LGPL).
#
# The non-UI logic (the document over a GroomSession, the OBJ reader, the camera, the RHI
# renderer) is a library of its own, groomlab_app_lib, so the tests cover it without a window.

if(GROOMLAB_QT_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${GROOMLAB_QT_DIR}")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network ShaderTools)
if(NOT TARGET groomlab_alembic)
    message(FATAL_ERROR "The app needs GROOMLAB_BUILD_ALEMBIC (scalps from caches, the Alembic export)")
endif()
qt_standard_project_setup()

# Qt's RHI dialect (GLSL 440, Vulkan-style bindings) compiled by qsb at build time to SPIR-V,
# HLSL 5.0 (Direct3D), MSL (Metal) and GLSL 120/150 (OpenGL), one .qsb per shader in the resources.
set(GROOMLAB_APP_SHADERS
    shaders/lines_lit.vert
    shaders/flat.vert
    shaders/flat.frag
    shaders/flat_colored.vert
    shaders/strands.vert
    shaders/strands_colored.vert
    shaders/dots.vert
    shaders/surface.vert
    shaders/surface.frag
    shaders/photo.vert
    shaders/photo.frag)
# The lookdev path fetches the strands from float textures in the vertex shader (texelFetch:
# GLSL 150 on OpenGL, older GL takes the line path), the same SPIR-V, HLSL and MSL as the rest.
set(GROOMLAB_LOOKDEV_SHADERS
    shaders/hair.vert
    shaders/hair_depth.frag
    shaders/hair_dom.frag
    shaders/hair_lookdev.frag
    shaders/hair_gouraud.frag
    shaders/points.vert
    shaders/surface_lookdev.vert
    shaders/surface_lookdev.frag
    shaders/surface_depth.frag
    shaders/surface_dom.frag
    shaders/fullscreen.vert
    shaders/background.frag
    shaders/accumulate.frag
    shaders/tonemap.frag
    shaders/sss_blur.frag
    shaders/hair_composite.frag)
# The hair's visibility path (lookdev_renderer.h, HairPath::Visibility): the strands rasterized,
# shaded and voxelized in compute shaders (GLSL 430 on OpenGL). hair_shading.glsl, hair_vis.glsl
# and hair_voxel.glsl are included (#include: qsb reads them beside the shader).
set(GROOMLAB_HAIR_COMPUTE_SHADERS
    shaders/hair_clear.comp
    shaders/hair_raster.comp
    shaders/hair_shade.comp
    shaders/hair_voxelize.comp
    shaders/hair_density.comp
    shaders/hair_volume.comp)

qt_add_library(groomlab_app_lib STATIC
    src/brush_session.cpp
    src/brush_session.h
    src/camera.cpp
    src/camera.h
    src/gpu_preview.cpp
    src/gpu_preview.h
    src/graph_presets.cpp
    src/graph_presets.h
    src/groom_document.cpp
    src/groom_document.h
    src/groom_document_lists.cpp
    src/groom_document_output.cpp
    src/groom_exports.cpp
    src/groom_tubes.cpp
    src/card_preview.cpp
    src/card_preview.h
    src/playback.cpp
    src/playback.h
    src/look.cpp
    src/look.h
    src/lookdev_renderer.cpp
    src/lookdev_renderer.h
    src/mesh_io.cpp
    src/mesh_io.h
    src/offscreen.cpp
    src/offscreen.h
    src/renderer.cpp
    src/renderer.h
    src/surface.cpp
    src/surface.h
    src/theme.cpp
    src/theme.h
    src/tool_view.cpp
    src/tool_view.h
    src/update_check.cpp
    src/update_check.h)
# The UI fonts (Inter for the words and, with tabular digits, the numbers; JetBrains Mono for code,
# hex colors and paths: google/fonts, SIL OFL 1.1, THIRD_PARTY_NOTICES.txt) as resources, so the
# app looks the same on every machine.
qt_add_resources(groomlab_app_lib "groomlab_app_fonts" PREFIX "/groomlab" BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "fonts/Inter[opsz,wght].ttf" "fonts/JetBrainsMono[wght].ttf")
# The Maya host's themes (Playful, Charcoal, Polar, Studio), read by theme.cpp: one set of files for
# both hosts' colors.
file(GLOB _groomlab_themes CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/hosts/maya/themes/*.json")
qt_add_resources(groomlab_app_lib "groomlab_app_theme" PREFIX "/groomlab/themes" BASE "${PROJECT_SOURCE_DIR}/hosts/maya/themes"
    FILES ${_groomlab_themes})
# The UI structure every host shares (engine/ui/*.json: menus, lists, choices...), embedded the same
# way: ":/groomlab/ui/menus.json" and so on (docs/UI_DEFINITIONS.md).
file(GLOB _groomlab_ui CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/engine/ui/*.json")
qt_add_resources(groomlab_app_lib "groomlab_app_ui" PREFIX "/groomlab/ui" BASE "${PROJECT_SOURCE_DIR}/engine/ui"
    FILES ${_groomlab_ui})
# Every shader set is PRECOMPILEd: on Windows qsb runs the SDK's fxc at build time and the .qsb
# carries DXBC, so Direct3D 11 and 12 load bytecode instead of compiling HLSL when the first frame
# makes its pipelines (the wolf's first frame 3.9 s -> 0.26 s; the pixels as before).
qt_add_shaders(groomlab_app_lib "groomlab_app_shaders"
    PREFIX "/groomlab"
    PRECOMPILE
    FILES ${GROOMLAB_APP_SHADERS})
# The strand lines' fragment shaders draw width as coverage hashed (unsigned integer hashes: GLSL
# 150 on OpenGL), and again under LINES_SAMPLE_MASK for a multisampled target (gl_SampleMask:
# GL_ARB_sample_shading on OpenGL).
qt_add_shaders(groomlab_app_lib "groomlab_strand_shaders"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    FILES shaders/strands.frag shaders/lines_lit.frag)
qt_add_shaders(groomlab_app_lib "groomlab_strand_mask_shaders"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    DEFINES LINES_SAMPLE_MASK
    FILES shaders/strands.frag shaders/lines_lit.frag
    OUTPUTS shaders/strands_mask.frag.qsb shaders/lines_lit_mask.frag.qsb)
qt_add_shaders(groomlab_app_lib "groomlab_lookdev_shaders"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    FILES ${GROOMLAB_LOOKDEV_SHADERS})
qt_add_shaders(groomlab_app_lib "groomlab_hair_compute_shaders"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "430"
    HLSL 50
    MSL 12
    FILES ${GROOMLAB_HAIR_COMPUTE_SHADERS})
# The surface shader again as the skin's subsurface prepass (the irradiance and depth targets
# the separable blur reads), the same source under SSS_PREPASS.
qt_add_shaders(groomlab_app_lib "groomlab_sss_prepass_shader"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    DEFINES SSS_PREPASS
    FILES shaders/surface_lookdev.frag
    OUTPUTS shaders/surface_sss.frag.qsb)
# The hair shading once per point (the interactive frame's per-point shading pass: the same
# source under HAIR_POINT_SHADE) and the ribbon vertex shader carrying the shaded color (HAIR_GOURAUD).
qt_add_shaders(groomlab_app_lib "groomlab_hair_point_shader"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    DEFINES HAIR_POINT_SHADE
    FILES shaders/hair_lookdev.frag
    OUTPUTS shaders/hair_point_shade.frag.qsb)
qt_add_shaders(groomlab_app_lib "groomlab_hair_gouraud_shader"
    PREFIX "/groomlab"
    PRECOMPILE
    GLSL "150"
    HLSL 50
    MSL 12
    DEFINES HAIR_GOURAUD
    FILES shaders/hair.vert
    OUTPUTS shaders/hair_gouraud.vert.qsb)
target_include_directories(groomlab_app_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The graph presets are the Maya host's files (hosts/maya/presets), read through the core's
# presets.h: a development build reads the source tree's; the build copies them beside the
# executable (groomlab_app_deploy), where an installed app finds them.
groomlab_path_literal(_shipped_presets "${PROJECT_SOURCE_DIR}/hosts/maya/presets")
target_compile_definitions(groomlab_app_lib PUBLIC "GROOMLAB_SHIPPED_PRESETS=${_shipped_presets}")
# The compound nodes shipped with every host (hosts/maya/nodes), found the same way.
groomlab_path_literal(_shipped_nodes "${PROJECT_SOURCE_DIR}/hosts/maya/nodes")
target_compile_definitions(groomlab_app_lib PUBLIC "GROOMLAB_SHIPPED_NODES=${_shipped_nodes}")
# The brush alphas shipped with every host (hosts/maya/alphas), found the same way.
groomlab_path_literal(_shipped_alphas "${PROJECT_SOURCE_DIR}/hosts/maya/alphas")
target_compile_definitions(groomlab_app_lib PUBLIC "GROOMLAB_SHIPPED_ALPHAS=${_shipped_alphas}")
# The sample meshes (GroomLab > Add Sample Mesh: Nika, Manny, the wolf; CC BY 4.0), the same way.
groomlab_path_literal(_shipped_meshes "${PROJECT_SOURCE_DIR}/hosts/maya/meshes")
target_compile_definitions(groomlab_app_lib PUBLIC "GROOMLAB_SHIPPED_MESHES=${_shipped_meshes}")
# QRhi's headers (rhi/qrhi.h) ship as Qt's "limited compatibility" API: they come with GuiPrivate.
# The GPU look preview's engine (engine/gpu/, Windows: Direct3D 11 and OpenGL) runs the graph's last
# nodes on QRhi's device while a param is dragged or guides are brushed (src/gpu_preview.h).
target_link_libraries(groomlab_app_lib
    PUBLIC groomlab::core groomlab::alembic groomlab::gpu Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Network
    PRIVATE nlohmann_json::nlohmann_json groomlab::warnings)
groomlab_precompile(groomlab_app_lib <nlohmann/json.hpp> <QByteArray> <QColor> <QElapsedTimer> <QImage>
    <QMatrix4x4> <QObject> <QPointF> <QSize> <QString> <QVector3D>)

# The widgets (the window, the floating UI, the panels and their editors, the node graph, the
# viewport widget) as a library of their own, so the tests drive the panels headless (the
# offscreen platform) without the executable's main.
qt_add_library(groomlab_app_ui STATIC
    src/app_actions.cpp
    src/app_actions.h
    src/app_graph_actions.cpp
    src/app_list_actions.cpp
    src/app_output_actions.cpp
    src/alpha_picker.cpp
    src/alpha_picker.h
    src/brush_panel.cpp
    src/brush_panel.h
    src/card_parts.cpp
    src/card_parts.h
    src/groom_tools.cpp
    src/groom_tools.h
    src/tube_panel.cpp
    src/tube_panel.h
    src/tube_shape_editor.cpp
    src/tube_shape_editor.h
    src/cards_panel.cpp
    src/cards_panel.h
    src/correctives_panel.cpp
    src/correctives_panel.h
    src/definitions.cpp
    src/definitions.h
    src/export_panel.cpp
    src/export_panel.h
    src/glass.cpp
    src/glass.h
    src/guides_panel.cpp
    src/guides_panel.h
    src/list_bar.cpp
    src/list_bar.h
    src/look_panel.cpp
    src/look_panel.h
    src/main_window.cpp
    src/main_window.h
    src/main_window_document.cpp
    src/main_window_headless.cpp
    src/main_window_internal.h
    src/main_window_panels.cpp
    src/main_window_properties.cpp
    src/main_window_scripting.cpp
    src/main_window_viewport.cpp
    src/main_window_output.cpp
    src/main_window_render.cpp
    src/maps_panel.cpp
    src/maps_panel.h
    src/marker_view.cpp
    src/marker_view.h
    src/marking_menu.cpp
    src/marking_menu.h
    src/node_graph.cpp
    src/node_graph.h
    src/overlay.cpp
    src/overlay.h
    src/param_editors.cpp
    src/param_editors.h
    src/param_panel.cpp
    src/param_panel.h
    src/photos_panel.cpp
    src/photos_panel.h
    src/properties_panel.cpp
    src/properties_panel.h
    src/app_photo_actions.cpp
    src/reference_window.cpp
    src/reference_window.h
    src/timeline.cpp
    src/timeline.h
    src/transfer_window.cpp
    src/transfer_window.h
    src/viewport.cpp
    src/viewport.h
    src/prman_render.cpp
    src/prman_render.h
    src/render_job.cpp
    src/render_job.h
    src/app_shell.cpp
    src/app_shell.h
    src/vray_render.cpp
    src/vray_render.h)
# The brush icons: GroomLab's own (the Maya plugin's, hosts/maya/icons), and the UI's (ui_*.svg, white
# strokes tinted at paint time), as resources (an icon added is found on the next build).
file(GLOB GROOMLAB_APP_ICONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg")
qt_add_resources(groomlab_app_ui "groomlab_app_icons" PREFIX "/groomlab" BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${GROOMLAB_APP_ICONS})
target_include_directories(groomlab_app_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(groomlab_app_ui
    PUBLIC groomlab_app_lib Qt6::Widgets
    PRIVATE nlohmann_json::nlohmann_json groomlab::warnings)
groomlab_precompile(groomlab_app_ui <nlohmann/json.hpp> <QCheckBox> <QColor> <QComboBox> <QElapsedTimer>
    <QHBoxLayout> <QImage> <QLabel> <QLineEdit> <QMouseEvent> <QPainter> <QPointF> <QScrollArea>
    <QSpinBox> <QString> <QTimer> <QVBoxLayout> <QWidget>)

qt_add_executable(groomlab_app src/main.cpp)
if(WIN32)
    target_sources(groomlab_app PRIVATE groomlab.rc)  # the executable's icon (groomlab.ico)
endif()
set_target_properties(groomlab_app PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "GroomLab")
target_link_libraries(groomlab_app PRIVATE groomlab_app_ui nlohmann_json::nlohmann_json groomlab::warnings)

# Beside the executable: the TBB runtime (as the CLI) and, on Windows, Qt's DLLs and platform
# plugins through windeployqt, so it runs from the build folder.
function(groomlab_app_deploy target)
    # Cleared first: a preset removed from the source must not linger beside the executable.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target}>/presets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/hosts/maya/presets" "$<TARGET_FILE_DIR:${target}>/presets"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target}>/nodes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/hosts/maya/nodes" "$<TARGET_FILE_DIR:${target}>/nodes"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target}>/alphas"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/hosts/maya/alphas" "$<TARGET_FILE_DIR:${target}>/alphas"
        COMMENT "Copying the graph presets, compound nodes and brush alphas beside ${target}")
    if(WIN32)
        if(GROOMLAB_TBB_RUNTIME)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime --no-system-d3d-compiler
                    --no-opengl-sw $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>
                    "$<TARGET_FILE:${target}>"
            COMMENT "Deploying Qt beside ${target}")
        # windeployqt copies the windows platform plugin alone; the tests and scripted runs use
        # the offscreen one (QT_QPA_PLATFORM=offscreen).
        if(TARGET Qt6::QOffscreenIntegrationPlugin)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin> "$<TARGET_FILE_DIR:${target}>/platforms/")
        endif()
    endif()
endfunction()
groomlab_app_deploy(groomlab_app)

# The app's package (tools/package_app.py: cmake --install --component App), one folder that runs
# wherever it is unzipped: the executable, its Qt (windeployqt over the installed copy, as above,
# without the DXC compiler the HLSL 5.0 shaders never need and without the TUIO touch plugin), the
# offscreen platform (scripted runs), the TBB runtime, the MSVC runtime app-local (Microsoft's
# redistributable DLLs from Visual Studio's Redist folder: InstallRequiredSystemLibraries), the
# graph presets, the notices with Qt's LGPL texts and README_BETA.txt (hosts/app/package).
set(GROOMLAB_APP_DIR "GroomLabApp")
install(TARGETS groomlab_app RUNTIME DESTINATION "${GROOMLAB_APP_DIR}" COMPONENT App)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/presets/" DESTINATION "${GROOMLAB_APP_DIR}/presets" COMPONENT App)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/nodes/" DESTINATION "${GROOMLAB_APP_DIR}/nodes" COMPONENT App)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/alphas/" DESTINATION "${GROOMLAB_APP_DIR}/alphas" COMPONENT App)
# The sample meshes (CC BY 4.0, REUSE.toml): their OBJs (the rigs are Maya's and Blender's).
install(DIRECTORY "${PROJECT_SOURCE_DIR}/hosts/maya/meshes/" DESTINATION "${GROOMLAB_APP_DIR}/meshes" COMPONENT App
        FILES_MATCHING PATTERN "*.obj")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/package/README_BETA.txt" DESTINATION "${GROOMLAB_APP_DIR}" COMPONENT App)
# GroomLab's RenderMan procedural (hosts/renderman, when RenderMan Pro Server's SDK is found) beside the
# app in procedurals/renderman, as the Maya module has it: Render > ... in RenderMan hands it to prman
# (prman_render.h). Copied beside the build's executable too, so a build runs the same way.
if(TARGET groomlab_renderman)
    add_dependencies(groomlab_app groomlab_renderman)
    add_custom_command(TARGET groomlab_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:groomlab_app>/procedurals/renderman"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:groomlab_renderman> $<TARGET_RUNTIME_DLLS:groomlab_renderman>
                "$<TARGET_FILE_DIR:groomlab_app>/procedurals/renderman"
        COMMAND_EXPAND_LISTS)
    install(TARGETS groomlab_renderman
        LIBRARY DESTINATION "${GROOMLAB_APP_DIR}/procedurals/renderman" COMPONENT App
        RUNTIME DESTINATION "${GROOMLAB_APP_DIR}/procedurals/renderman" COMPONENT App)
    if(WIN32 AND GROOMLAB_TBB_RUNTIME)
        install(FILES $<TARGET_RUNTIME_DLLS:groomlab_renderman> DESTINATION "${GROOMLAB_APP_DIR}/procedurals/renderman" COMPONENT App)
    endif()
endif()
groomlab_install_legal("${GROOMLAB_APP_DIR}" App)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/package/licenses/" DESTINATION "${GROOMLAB_APP_DIR}/licenses" COMPONENT App)
if(WIN32)
    if(GROOMLAB_TBB_RUNTIME)
        install(FILES $<TARGET_RUNTIME_DLLS:groomlab_app> DESTINATION "${GROOMLAB_APP_DIR}" COMPONENT App)
    endif()
    install(CODE "
        execute_process(COMMAND \"$<TARGET_FILE:Qt6::windeployqt>\" $<IF:$<CONFIG:Debug>,--debug,--release>
                                --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler
                                --no-opengl-sw --skip-plugin-types generic
                                \"\${CMAKE_INSTALL_PREFIX}/${GROOMLAB_APP_DIR}/$<TARGET_FILE_NAME:groomlab_app>\"
                        RESULT_VARIABLE _deploy OUTPUT_QUIET)
        if(NOT _deploy EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed (\${_deploy}) on the installed app\")
        endif()" COMPONENT App)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        install(FILES $<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin> DESTINATION "${GROOMLAB_APP_DIR}/platforms" COMPONENT App)
    endif()
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${GROOMLAB_APP_DIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT App)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endif()

# The app's tests live with the others (tests/hosts/app) and build here, beside the executable:
# they link groomlab_app_ui and run with the Qt this folder deploys.
if(GROOMLAB_BUILD_TESTS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/hosts/app" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
