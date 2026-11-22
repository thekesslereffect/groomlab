# SPDX-License-Identifier: Apache-2.0
# Research prototypes (docs/research/*.md): each idea measured against the current core on realistic
# data. Built only with -DGROOMLAB_BUILD_RESEARCH=ON; no test, no install, nothing ships.

function(groomlab_research name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE groomlab::core)
    target_compile_definitions(${name} PRIVATE "RESEARCH_DATA=\"${PROJECT_SOURCE_DIR}/tests/data\"")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W3 /permissive-)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
    if(WIN32 AND GROOMLAB_TBB_RUNTIME)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${name}> $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

groomlab_research(research_baked_binding baked_binding/bench_baked_binding.cpp)

groomlab_research(research_curl_sweeps curl_sweeps/bench_curl_sweeps.cpp)
target_link_libraries(research_curl_sweeps PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(research_curl_sweeps PRIVATE "RESEARCH_PRESETS=\"${PROJECT_SOURCE_DIR}/engine/assets/presets\"")

groomlab_research(research_clump_lod clump_lod/bench_clump_lod.cpp)
target_compile_definitions(research_clump_lod PRIVATE "RESEARCH_PRESETS=\"${PROJECT_SOURCE_DIR}/engine/assets/presets\"")

groomlab_research(research_curl_b1 curl_sweeps/measure_curl_b1.cpp)
target_link_libraries(research_curl_b1 PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(research_curl_b1 PRIVATE "RESEARCH_PRESETS=\"${PROJECT_SOURCE_DIR}/engine/assets/presets\"")
