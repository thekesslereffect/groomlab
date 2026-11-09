# SPDX-License-Identifier: Apache-2.0
# Arnold procedural (OR-3): renders groom files at render time. Needs only the
# Arnold SDK (found under MTOA_LOCATION), not Maya, so farms can load it.
add_library(groomlab_procedural MODULE groomlab_procedural.cpp)
target_link_libraries(groomlab_procedural PRIVATE groomlab::core groomlab::alembic Arnold::Arnold)
target_include_directories(groomlab_procedural PRIVATE include)
set_target_properties(groomlab_procedural PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/procedurals"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/procedurals")
if(APPLE)
    set_target_properties(groomlab_procedural PROPERTIES SUFFIX ".dylib")  # what Arnold loads from ARNOLD_PLUGIN_PATH on macOS
endif()
# No groomlab::warnings: Arnold's headers trip /W4.

# kick and farm renders run without Maya's DLLs, and Windows does not search a
# plugin's own folder for its dependencies. So TBB is delay-loaded, installed
# beside the procedural, and loaded from there by node_loader when the host
# (unlike Maya) has not loaded it already.
if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    target_link_options(groomlab_procedural PRIVATE "/DELAYLOAD:$<TARGET_FILE_NAME:groomlab::tbb>")
    target_link_libraries(groomlab_procedural PRIVATE delayimp)
    target_compile_definitions(groomlab_procedural PRIVATE "GROOMLAB_TBB_DLL=L\"$<TARGET_FILE_NAME:groomlab::tbb>\"")
    add_custom_command(TARGET groomlab_procedural POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:groomlab_procedural> $<TARGET_FILE_DIR:groomlab_procedural>
        COMMAND_EXPAND_LISTS)
    install(FILES $<TARGET_RUNTIME_DLLS:groomlab_procedural> DESTINATION "GroomLab/procedurals" COMPONENT GroomLab)
endif()

# Installed into the Maya module; GroomLab.mod puts it on ARNOLD_PLUGIN_PATH.
install(TARGETS groomlab_procedural
    LIBRARY DESTINATION "GroomLab/procedurals" COMPONENT GroomLab
    RUNTIME DESTINATION "GroomLab/procedurals" COMPONENT GroomLab)
# Linux: node_loader exported alone, libtbb.so.12 beside it ($ORIGIN), as kick on a farm loads it.
# macOS: Maya.app/Contents/MacOS first (@executable_path/../MacOS from Maya's MacOS/Maya and from
# bin/mayapy alike), where Maya keeps the TBB it has loaded, so the procedural shares it inside Maya
# rather than loading the copy beside it as a second TBB (dyld does not merge two files by their
# install name); kick (mtoa/<year>/bin) has no ../MacOS and takes the copy (@loader_path).
if(APPLE)
    set_property(TARGET groomlab_procedural APPEND PROPERTY INSTALL_RPATH "@executable_path/../MacOS")
endif()
groomlab_hide_static_symbols(groomlab_procedural)
groomlab_install_runtime(groomlab_procedural "GroomLab/procedurals" GroomLab)
