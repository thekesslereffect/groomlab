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
if(APPLE)
    # The display's pixels per point under the cursor (the brush's radius in points: display_scale.h).
    target_sources(GroomLab PRIVATE src/display_scale.cpp)
    target_link_libraries(GroomLab PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()
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

# V-Ray: a translator V-Ray for Maya loads (vraytr_*.dll from VRAY_FOR_MAYA<year>_TRANSLATORS_PATH)
# that renders groomLabShape as hair. It builds against the V-Ray for Maya install of this Maya
# version (its vray/include, vray/include/vray_maya_sdk and vray/lib: Chaos's SDK, never in this
# repository), V-Ray 7's by default.
if(GROOMLAB_BUILD_VRAY)
    set(_vray_default "")
    if(WIN32)
        set(_program_files "$ENV{ProgramW6432}")
        if(NOT _program_files)
            set(_program_files "$ENV{ProgramFiles}")
        endif()
        file(TO_CMAKE_PATH "${_program_files}/Chaos/V-Ray/Maya ${GROOMLAB_MAYA_YEAR} for x64" _vray_default)
    elseif(APPLE)
        set(_vray_default "/Applications/Chaos/V-Ray/Maya${GROOMLAB_MAYA_YEAR}")
    endif()
    set(VRAY_FOR_MAYA_LOCATION "${_vray_default}" CACHE PATH
        "V-Ray for Maya's install for this Maya version (holding vray/include/vray_maya_sdk)")
    if(EXISTS "${VRAY_FOR_MAYA_LOCATION}/vray/include/vray_maya_sdk/translator_interface.h")
        message(STATUS "V-Ray for Maya at ${VRAY_FOR_MAYA_LOCATION}")
        if(APPLE)
            # macOS: a bundle named libvraytr_groomlab.so, as V-Ray for Maya's own translators are (it
            # looks for <lib>vraytr_*.<so> in VRAY_FOR_MAYA<year>_TRANSLATORS_PATH).
            add_library(vraytr_groomlab MODULE vray/groom_translator.cpp)
            # vutils and plugman as on Windows (libvutils_s.a, libplugman_s.a); V-Ray's own symbols
            # (vray.lib's on Windows) come from the libvray.dylib V-Ray has loaded when it loads a
            # translator: its translators link no libvray, which no rpath of the module would find.
            # (ApplicationServices: vutils' process calls, as V-Ray's samples link it.)
            target_link_libraries(vraytr_groomlab PRIVATE groomlab_maya_inputs
                "${VRAY_FOR_MAYA_LOCATION}/vray/lib/libvutils_s.a" "${VRAY_FOR_MAYA_LOCATION}/vray/lib/libplugman_s.a"
                "-framework ApplicationServices")
            target_link_options(vraytr_groomlab PRIVATE "LINKER:-undefined,dynamic_lookup")
            # No __SSE*__: V-Ray's defines.h sets NO_SSE on arm64 itself.
            target_compile_definitions(vraytr_groomlab PRIVATE _REENTRANT NOMINMAX VUTILS_UTF8 END_USER_BUILD)
            set_target_properties(vraytr_groomlab PROPERTIES PREFIX "lib" SUFFIX ".so")
        else()
            add_library(vraytr_groomlab SHARED vray/groom_translator.cpp)
            # V-Ray's base libraries, as V-Ray's samples link them: vutils and plugman (PluginBase: the
            # keyed points parameter derives from V-Ray's), and vray.dll's import library (the parameter's
            # texture interfaces), which V-Ray has loaded whenever it loads a translator.
            target_link_libraries(vraytr_groomlab PRIVATE groomlab_maya_inputs
                "${VRAY_FOR_MAYA_LOCATION}/vray/lib/vutils_s.lib" "${VRAY_FOR_MAYA_LOCATION}/vray/lib/plugman_s.lib"
                "${VRAY_FOR_MAYA_LOCATION}/vray/lib/vray.lib")
            # V-Ray's headers as its SDK's own builds compile them (samples/enduser_cmake).
            target_compile_definitions(vraytr_groomlab PRIVATE _REENTRANT NOMINMAX VUTILS_UTF8 END_USER_BUILD
                __SSE__ __SSE2__ __SSE3__ __SSSE3__ __SSE4_1__ __SSE4_2__)
            set_target_properties(vraytr_groomlab PROPERTIES PREFIX "")
        endif()
        target_include_directories(vraytr_groomlab SYSTEM PRIVATE
            "${VRAY_FOR_MAYA_LOCATION}/vray/include" "${VRAY_FOR_MAYA_LOCATION}/vray/include/vray_maya_sdk")
        set_target_properties(vraytr_groomlab PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vray"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vray")
        # No groomlab::warnings here: V-Ray's headers trip /permissive- and /W4.
    elseif(VRAY_FOR_MAYA_LOCATION)
        message(STATUS "V-Ray for Maya ${GROOMLAB_MAYA_YEAR} not found at '${VRAY_FOR_MAYA_LOCATION}': no V-Ray translator")
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
if(TARGET vraytr_groomlab)
    string(APPEND GROOMLAB_MOD_EXTENSIONS "VRAY_FOR_MAYA${GROOMLAB_MAYA_YEAR}_TRANSLATORS_PATH+:=${GROOMLAB_MAYA_BIN}vray\n")
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
# Retired themes an earlier install left in the module (install never deletes): Polar (replaced by Paper).
install(CODE "foreach(_gone polar.json)
    file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/${GROOMLAB_MODULE_DIR}/themes/\${_gone}\")
endforeach()" COMPONENT GroomLab)
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
if(TARGET vraytr_groomlab)
    install(TARGETS vraytr_groomlab
        RUNTIME DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}vray" COMPONENT GroomLab
        LIBRARY DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}vray" COMPONENT GroomLab)
endif()
if(TARGET groomLabTranslator)
    install(TARGETS groomLabTranslator
        RUNTIME DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}extensions" COMPONENT GroomLab
        LIBRARY DESTINATION "${GROOMLAB_MODULE_DIR}/${GROOMLAB_MAYA_BIN}extensions" COMPONENT GroomLab)
endif()
