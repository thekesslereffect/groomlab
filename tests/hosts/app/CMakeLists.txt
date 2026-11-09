# SPDX-License-Identifier: Apache-2.0
# The app's tests (ctest label quick, with the core's): the non-UI logic (the document over a
# session, the OBJ reader, the camera), a headless render of the sample groom through QRhi
# into a texture, read back and checked for pixels, the panels (the param editors, the look
# card, the maps panel) driven without a window, and the live benchmark. No window:
# QT_QPA_PLATFORM=offscreen.
add_executable(groomlab_app_tests test_app.cpp test_panels.cpp test_definitions.cpp test_graph_editor.cpp test_tubes.cpp test_lists.cpp test_properties.cpp test_output.cpp test_marking_menu.cpp test_subdivision.cpp bench_app.cpp)
target_link_libraries(groomlab_app_tests PRIVATE groomlab_app_ui groomlab::warnings GTest::gtest nlohmann_json::nlohmann_json)
groomlab_path_literal(_test_data "${PROJECT_SOURCE_DIR}/tests/data")
target_compile_definitions(groomlab_app_tests PRIVATE "GROOMLAB_TEST_DATA=${_test_data}")
groomlab_precompile(groomlab_app_tests <gtest/gtest.h> <nlohmann/json.hpp> <QImage> <QSize> <QString>)
groomlab_app_deploy(groomlab_app_tests)
# Every run of the app here (the gtest suite, the executable's scripted runs) keeps its settings
# (recent files, the theme, autosaves, units) in a folder of the build's own, GROOMLAB_SETTINGS_DIR:
# never the user's %APPDATA%\GroomLab\GroomLab.ini. A folder a shard (they run side by side).
set(GROOMLAB_APP_TEST_SETTINGS "${CMAKE_CURRENT_BINARY_DIR}/settings")
set(_app_env "QT_QPA_PLATFORM=offscreen;GROOMLAB_SETTINGS_DIR=${GROOMLAB_APP_TEST_SETTINGS}")
groomlab_gtest_shards(groomlab_app_tests app 4 "" "quick;app" QT_QPA_PLATFORM=offscreen)
foreach(_shard RANGE 0 3)
    set_property(TEST app.shard${_shard} APPEND PROPERTY ENVIRONMENT "GROOMLAB_SETTINGS_DIR=${GROOMLAB_APP_TEST_SETTINGS}/shard${_shard}")
endforeach()
# The scripted exports and card bake through the executable's flags (no window).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME app.cli_exports
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/cli_exports.py" "$<TARGET_FILE:groomlab_app>"
                     "${PROJECT_SOURCE_DIR}/tests/data/strokes/strokes.groom" "${CMAKE_CURRENT_BINARY_DIR}/cli_exports")
    set_tests_properties(app.cli_exports PROPERTIES LABELS "quick;app" TIMEOUT 600
                         ENVIRONMENT "${_app_env}")
    # Render > ... in V-Ray (vray_render.h) through the executable's flags, when V-Ray Standalone is
    # installed: a frame and a three-frame turntable of the head groom. V-Ray's license is checked
    # out per render: alone.
    if(APPLE)  # vray.bin inside the install's VRay.app
        file(GLOB _vray_mac_paths "/Applications/Chaos/V-Ray/Standalone_for_*/VRay.app/Contents/MacOS")
        find_program(GROOMLAB_APP_VRAY vray.bin PATHS "$ENV{VRAY_PATH}" ${_vray_mac_paths} NO_CACHE)
    else()
        find_program(GROOMLAB_APP_VRAY vray
            PATHS "$ENV{ProgramFiles}/Chaos/V-Ray/Standalone for x64/bin" "$ENV{VRAY_PATH}" NO_CACHE)
    endif()
    if(GROOMLAB_APP_VRAY)
        add_test(NAME app.vray_render
                 COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/vray_render.py" "$<TARGET_FILE:groomlab_app>"
                         "${GROOMLAB_APP_VRAY}" "${CMAKE_CURRENT_BINARY_DIR}/vray_render")
        set_tests_properties(app.vray_render PROPERTIES LABELS "app;vray" RUN_SERIAL TRUE TIMEOUT 900
                             ENVIRONMENT "${_app_env}")
    endif()
    # Render > ... in RenderMan (prman_render.h) through the executable's flags, when prman and the
    # RenderMan procedural (copied beside the app's build) are there: a frame and a three-frame
    # turntable of the head groom. prman's license is one per machine: alone.
    if(TARGET groomlab_renderman AND RenderMan_PRMAN)
        add_test(NAME app.prman_render
                 COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/prman_render.py" "$<TARGET_FILE:groomlab_app>"
                         "${RenderMan_PRMAN}" "${CMAKE_CURRENT_BINARY_DIR}/prman_render")
        set_tests_properties(app.prman_render PROPERTIES LABELS "app;renderman" RUN_SERIAL TRUE TIMEOUT 900
                             ENVIRONMENT "${_app_env}")
    endif()
    # The app's package (tools/package_app.py --stage-only): the App component installed into a
    # scratch folder and checked against the manifest (every file it needs, nothing it must not
    # ship, every DLL each binary imports beside it or in Windows, Qt's files Qt's own).
    if(WIN32)
        add_test(NAME app.package_manifest
                 COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/package_app.py" --build "${CMAKE_BINARY_DIR}"
                         --config $<CONFIG> --cmake "${CMAKE_COMMAND}" --out "${CMAKE_CURRENT_BINARY_DIR}/package_manifest" --stage-only)
        set_tests_properties(app.package_manifest PROPERTIES LABELS "quick;app" TIMEOUT 300
                             ENVIRONMENT "GROOMLAB_SETTINGS_DIR=${GROOMLAB_APP_TEST_SETTINGS}")
    endif()
endif()
