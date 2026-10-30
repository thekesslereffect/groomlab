# SPDX-License-Identifier: Apache-2.0
find_package(Maya REQUIRED)
message(STATUS "Maya API ${Maya_API_VERSION} at ${MAYA_LOCATION}")

# The Maya version this build is for (2022, 2023...): a plugin is binary compatible within one
# version only, so the module holds one build per version, Maya 2022's at its root (plug-ins/,
# python/, extensions/: the layout every earlier release had, which their updaters check) and every
# other's in maya<year>/ (plug-ins/, python/, extensions/), picked by the .mod's MAYAVERSION
# sections (module/GroomLab.mod.in, tools/package_release.py).
string(SUBSTRING "${Maya_API_VERSION}" 0 4 GROOMLAB_MAYA_YEAR)
set(GROOMLAB_MAYA_YEAR "${GROOMLAB_MAYA_YEAR}" CACHE INTERNAL "The Maya version the plugin is built for")
if(GROOMLAB_MAYA_YEAR STREQUAL "2022")
    set(GROOMLAB_MAYA_BIN "")
else()
    set(GROOMLAB_MAYA_BIN "maya${GROOMLAB_MAYA_YEAR}/")
endif()

# Maya-to-core conversion shared by the plugin and render translators.
add_library(groomlab_maya_inputs STATIC src/maya_inputs.cpp)
target_include_directories(groomlab_maya_inputs PUBLIC src)
target_link_libraries(groomlab_maya_inputs PUBLIC groomlab::core Maya::Maya PRIVATE groomlab::warnings nlohmann_json::nlohmann_json)
if(TARGET groomlab_alembic)
    # registerHostOperators() adds ImportCurves; the translator gets it too.
    target_link_libraries(groomlab_maya_inputs PUBLIC groomlab::alembic)
    target_compile_definitions(groomlab_maya_inputs PRIVATE GROOMLAB_HAS_ALEMBIC=1)
endif()

# Qt for tablet pen pressure (pen_pressure.cpp): Maya ships its Qt headers
# zipped in the devkit; unpack them once. Without them the pressure is 1.
# Maya 2022 to 2024: Qt 5.15 (include/qt_5.15.2_vc14-include.zip, lib/Qt5*.lib);
# Maya 2025 and later: Qt 6 (the devkit's Qt.zip: Qt/include, Qt/lib/Qt6*.lib).
add_library(groomlab_maya_qt STATIC src/pen_pressure.cpp)
target_include_directories(groomlab_maya_qt PUBLIC src)
file(GLOB _qt5_zip "${Maya_INCLUDE_DIR}/qt_5*-include.zip")
set(_qt6_zip "${MAYA_LOCATION}/Qt.zip")
if(WIN32 AND _qt5_zip)
    list(GET _qt5_zip 0 _qt5_zip)
    set(_qt_dir "${CMAKE_BINARY_DIR}/maya-qt-include")
    if(NOT EXISTS "${_qt_dir}/QtCore/qobject.h")
        message(STATUS "Unpacking Maya's Qt headers to ${_qt_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_qt5_zip}" DESTINATION "${_qt_dir}")
    endif()
    target_include_directories(groomlab_maya_qt SYSTEM PRIVATE "${_qt_dir}")
    target_compile_definitions(groomlab_maya_qt PRIVATE GROOMLAB_HAS_QT=1)
    foreach(_qt Qt5Core Qt5Gui Qt5Widgets)
        target_link_libraries(groomlab_maya_qt PRIVATE "${Maya_LIBRARY_DIR}/${_qt}.lib")
    endforeach()
elseif(WIN32 AND (EXISTS "${_qt6_zip}" OR EXISTS "${MAYA_LOCATION}/Qt/include/QtCore/qobject.h"))
    if(EXISTS "${MAYA_LOCATION}/Qt/include/QtCore/qobject.h")
        set(_qt_dir "${MAYA_LOCATION}")  # Qt.zip unpacked in place (its Qt/include, Qt/lib)
    else()
        set(_qt_dir "${CMAKE_BINARY_DIR}/maya-qt6")
    endif()
    if(NOT EXISTS "${_qt_dir}/Qt/include/QtCore/qobject.h")
        message(STATUS "Unpacking Maya's Qt 6 headers to ${_qt_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_qt6_zip}" DESTINATION "${_qt_dir}"
             PATTERNS "Qt/include/QtCore/*" "Qt/include/QtGui/*" "Qt/include/QtWidgets/*"
                      "Qt/lib/Qt6Core.lib" "Qt/lib/Qt6Gui.lib" "Qt/lib/Qt6Widgets.lib")
    endif()
    target_include_directories(groomlab_maya_qt SYSTEM PRIVATE "${_qt_dir}/Qt/include")
    target_compile_definitions(groomlab_maya_qt PRIVATE GROOMLAB_HAS_QT=1)
    # Qt 6 checks __cplusplus, which MSVC reports right only with /Zc:__cplusplus.
    target_compile_options(groomlab_maya_qt PRIVATE /Zc:__cplusplus /permissive-)
    foreach(_qt Qt6Core Qt6Gui Qt6Widgets)
        target_link_libraries(groomlab_maya_qt PRIVATE "${_qt_dir}/Qt/lib/${_qt}.lib")
    endforeach()
else()
    message(STATUS "Maya Qt headers not found: tablet pen pressure disabled")
endif()

maya_add_plugin(GroomLab
    src/active_groom.cpp
    src/asset_files.cpp
    src/brush_tool.cpp
    src/convert_cmd.cpp
    src/crash_log.cpp
    src/display_theme.cpp
    src/export_cmds.cpp
    src/gpu_lines.cpp
    src/gpu_preview.cpp
    src/groom_draw_override.cpp
    src/groom_draw_override_fragments.cpp
    src/groom_draw_override_gpu.cpp
    src/groom_draw_override_items.cpp
    src/groom_draw_override_lod.cpp
    src/groom_draw_override_scene.cpp
    src/groom_draw_override_shaders.cpp
    src/hair_shading.cpp
    src/groom_shape.cpp
    src/groom_shape_strand_mesh.cpp
    src/guides_cmd.cpp
    src/import_cmd.cpp
    src/marker_set.cpp
    src/file_node.cpp
    src/parts_cmd.cpp
    src/scalp_maps_cmd.cpp
    src/transfer_cmd.cpp
    src/plugin.cpp
    src/reference_tool.cpp
    src/select_tool.cpp
    src/sim_cmd.cpp
    src/tool_view.cpp
    src/version_cmd.cpp
)
target_link_libraries(GroomLab PRIVATE groomlab_maya_inputs groomlab_maya_qt groomlab::warnings
    nlohmann_json::nlohmann_json)  # graphStatsJson
# The Maya API headers most of the plugin's files include (none that pulls in windows.h).
groomlab_precompile(GroomLab <nlohmann/json.hpp>
    <maya/MArgDatabase.h> <maya/MArgList.h> <maya/MDagPath.h> <maya/MDGModifier.h> <maya/MDoubleArray.h>
    <maya/MFnDagNode.h> <maya/MFnDependencyNode.h> <maya/MFnMesh.h> <maya/MGlobal.h> <maya/MIntArray.h>
    <maya/MMatrix.h> <maya/MObject.h> <maya/MPlug.h> <maya/MPoint.h> <maya/MPxCommand.h>
    <maya/MSelectionList.h> <maya/MStatus.h> <maya/MString.h> <maya/MStringArray.h> <maya/MSyntax.h>)
if(WIN32)
    target_link_libraries(GroomLab PRIVATE Dbghelp Version)  # crash_log: stacks with symbols, module versions
    target_link_libraries(GroomLab PRIVATE groomlab::gpu opengl32)  # the GPU look preview (engine/gpu/); opengl32: glFinish (-sync)
else()
    # The GPU look preview's engine without a backend (engine/gpu/: macOS, Linux): drags, strokes and
    # the lit lines' frames are the CPU's (gpu_preview.cpp, gpu_lines.cpp).
    target_link_libraries(GroomLab PRIVATE groomlab::gpu)
endif()
if(TARGET groomlab_alembic)
    target_compile_definitions(GroomLab PRIVATE GROOMLAB_HAS_ALEMBIC=1)
endif()
# The hair shading's lobes, one source with the standalone app's (engine/gpu/shaders/hair_bsdf.glsl):
# its text embedded at build time (hair_bsdf_text.h, kHairBsdfText), for hair_shading.cpp's effects.
set(_hair_bsdf "${PROJECT_SOURCE_DIR}/engine/gpu/shaders/hair_bsdf.glsl")
set(_hair_bsdf_h "${CMAKE_CURRENT_BINARY_DIR}/generated/hair_bsdf_text.h")
add_custom_command(OUTPUT "${_hair_bsdf_h}"
    COMMAND "${CMAKE_COMMAND}" -DSOURCE=${_hair_bsdf} -DHEADER=${_hair_bsdf_h} -DNAME=kHairBsdfText
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake"
    DEPENDS "${_hair_bsdf}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake"
    COMMENT "Embedding hair_bsdf.glsl")
target_sources(GroomLab PRIVATE "${_hair_bsdf_h}")
# The strand lines' width as coverage, one source with the app's (engine/gpu/shaders/line_coverage.glsl):
# embedded the same way (line_coverage_text.h, kLineCoverageText) for the lines and hair effects and
# the lit lines' coverage fragment.
set(_line_coverage "${PROJECT_SOURCE_DIR}/engine/gpu/shaders/line_coverage.glsl")
set(_line_coverage_h "${CMAKE_CURRENT_BINARY_DIR}/generated/line_coverage_text.h")
add_custom_command(OUTPUT "${_line_coverage_h}"
    COMMAND "${CMAKE_COMMAND}" -DSOURCE=${_line_coverage} -DHEADER=${_line_coverage_h} -DNAME=kLineCoverageText
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake"
    DEPENDS "${_line_coverage}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake"
    COMMENT "Embedding line_coverage.glsl")
target_sources(GroomLab PRIVATE "${_line_coverage_h}")
target_include_directories(GroomLab PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Arnold: MtoA extension that renders groomLabShape as curves.
if(GROOMLAB_BUILD_ARNOLD)
    find_package(MtoA)
    if(MtoA_FOUND)
        message(STATUS "MtoA ${MtoA_VERSION} at ${MTOA_LOCATION}")
        add_library(groomLabTranslator SHARED arnold/groom_translator.cpp)
        target_link_libraries(groomLabTranslator PRIVATE groomlab_maya_inputs MtoA::MtoA)
        target_include_directories(groomLabTranslator PRIVATE "${PROJECT_SOURCE_DIR}/hosts/arnold/include")
        set_target_properties(groomLabTranslator PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions")
        # No groomlab::warnings here: MtoA headers trip /permissive- and /W4.
    else()
        message(WARNING "GROOMLAB_BUILD_ARNOLD is ON but MtoA was not found at '${MTOA_LOCATION}'")
    endif()
endif()

# Maya module layout: <prefix>/GroomLab.mod + <prefix>/GroomLab/{plug-ins,scripts,python,extensions}
# (Maya 2022's binaries) and <prefix>/GroomLab/maya<year>/{plug-ins,python,extensions} (another
# version's). Maya only scans the top level of MAYA_MODULE_PATH for .mod files; the module path
# inside the file is relative to the file itself. The .mod holds this build's MAYAVERSION section;
# tools/package_release.py joins every version's into one.
set(GROOMLAB_MODULE_DIR "GroomLab")
if(GROOMLAB_MAYA_BIN)
    set(GROOMLAB_MOD_PLUGINS "plug-ins: ${GROOMLAB_MAYA_BIN}plug-ins\n")
else()
    set(GROOMLAB_MOD_PLUGINS "")
endif()
if(TARGET groomLabTranslator)
    set(GROOMLAB_MOD_EXTENSIONS "MTOA_EXTENSIONS_PATH+:=${GROOMLAB_MAYA_BIN}extensions\n")
else()
    set(GROOMLAB_MOD_EXTENSIONS "")
endif()
configure_file(module/GroomLab.mod.in "${CMAKE_CURRENT_BINARY_DIR}/GroomLab.mod" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/GroomLab.mod" DESTINATION "." COMPONENT GroomLab)
install(TARGETS GroomLab
    RUNTIME DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}plug-ins" COMPONENT GroomLab
    LIBRARY DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}plug-ins" COMPONENT GroomLab)
install(DIRECTORY scripts/ DESTINATION "${GROOMLAB_MODULE_DIR}/scripts" COMPONENT GroomLab
    PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY presets/ DESTINATION "${GROOMLAB_MODULE_DIR}/presets" COMPONENT GroomLab)
install(DIRECTORY icons/ DESTINATION "${GROOMLAB_MODULE_DIR}/icons" COMPONENT GroomLab)  # on XBMLANGPATH
install(DIRECTORY expressions/ DESTINATION "${GROOMLAB_MODULE_DIR}/expressions" COMPONENT GroomLab)  # the expression library's examples
install(DIRECTORY themes/ DESTINATION "${GROOMLAB_MODULE_DIR}/themes" COMPONENT GroomLab)  # UI themes (JSON)
# The UI font of GroomLab's windows (the app's Inter, SIL OFL 1.1: its license travels with it), loaded
# by scripts/groomlab/ui/theme.py for GroomLab's own windows only.
install(FILES "${PROJECT_SOURCE_DIR}/hosts/app/fonts/Inter[opsz,wght].ttf" "${PROJECT_SOURCE_DIR}/hosts/app/fonts/OFL-Inter.txt"
    DESTINATION "${GROOMLAB_MODULE_DIR}/fonts" COMPONENT GroomLab)
# The UI structure every host shares (engine/ui: menus, lists, choices...), read by scripts/groomlab/ui/definitions.py.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/ui/" DESTINATION "${GROOMLAB_MODULE_DIR}/ui" COMPONENT GroomLab)
install(DIRECTORY nodes/ DESTINATION "${GROOMLAB_MODULE_DIR}/nodes" COMPONENT GroomLab)  # compound nodes (.glnode) shipped with every host
install(DIRECTORY alphas/ DESTINATION "${GROOMLAB_MODULE_DIR}/alphas" COMPONENT GroomLab)  # brush alphas shipped with every host
# The sample meshes (CC BY 4.0): Nika, Manny, the wolf, Buddy (its rig, buddy_rig.ma; the .fbx is Blender's).
install(DIRECTORY meshes/ DESTINATION "${GROOMLAB_MODULE_DIR}/meshes" COMPONENT GroomLab PATTERN "*.fbx" EXCLUDE)
# GroomLab's license and NOTICE, and the licenses of the open-source code inside the binaries and of
# tbb.dll, shipped with them.
groomlab_install_legal("${GROOMLAB_MODULE_DIR}" GroomLab)
# The plugin's Python (scripts/groomlab) imports the core's module for what the hosts share
# (card presets, map names, card bake files, graph edits): it ships beside the plugin, on the
# module's PYTHONPATH, and builds with it.
if(TARGET groomlab_core_py)
    add_dependencies(GroomLab groomlab_core_py)
    install(TARGETS groomlab_core_py LIBRARY DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}python" COMPONENT GroomLab)
else()
    message(WARNING "GROOMLAB_BUILD_PYTHON is off: the plugin's Python (scripts/groomlab) needs the groomlab_core module")
endif()
if(TARGET groomLabTranslator)
    install(TARGETS groomLabTranslator
        RUNTIME DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}extensions" COMPONENT GroomLab
        LIBRARY DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}extensions" COMPONENT GroomLab)
endif()
