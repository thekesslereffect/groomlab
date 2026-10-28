# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)

# MSVC debug information as a target property (CMAKE_MSVC_DEBUG_INFORMATION_FORMAT), for this
# project and the fetched ones: cmake/GroomLabBuildSpeed.cmake picks /Z7 for Ninja builds.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
endif()

project(GroomLab VERSION 0.35.0 LANGUAGES CXX)

# Maya 2022 devkit targets C++17 (Visual Studio 2019 / v142 ABI; v143 is binary compatible).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GROOMLAB_BUILD_TESTS   "Build core unit tests (GoogleTest)"                ON)
option(GROOMLAB_BUILD_PYTHON  "Build the pybind11 module (groomlab_core)"         ON)
option(GROOMLAB_BUILD_CLI     "Build the headless groomlab-cli tool"              ON)
option(GROOMLAB_BUILD_CAPI    "Build the C API library (groomlab, engine/capi)"   ON)
option(GROOMLAB_BUILD_BLENDER_HELPER "Build groomlab-helper, the Blender add-on's core process" ON)
option(GROOMLAB_BUILD_ALEMBIC "Build Alembic export (fetches Alembic + Imath)"    ON)
option(GROOMLAB_BUILD_MAYA    "Build the Maya plugin (needs MAYA_LOCATION)"       OFF)
option(GROOMLAB_BUILD_ARNOLD  "Build the MtoA translator (needs MTOA_LOCATION)"   ON)
# The Arnold procedural depends on Arnold alone: one build (against the oldest Arnold 7 SDK, Maya
# 2022's MtoA) loads in every Arnold 7, so the other Maya versions' presets leave it out.
option(GROOMLAB_BUILD_PROCEDURAL "Build the Arnold procedural (needs the Arnold SDK under MTOA_LOCATION)" ON)
# The RenderMan procedural (hosts/renderman): built when a RenderMan Pro Server SDK is found under
# GROOMLAB_RMANTREE. Like the Arnold procedural it depends on its renderer alone, so one build (Maya
# 2022's) ships it; the other Maya versions' presets leave it out.
option(GROOMLAB_BUILD_RENDERMAN "Build the RenderMan procedural (needs RenderMan Pro Server under GROOMLAB_RMANTREE)" ON)
option(GROOMLAB_USE_TBB       "Use TBB for parallel loops (serial otherwise)"     ON)
# The standalone app (hosts/app/: Qt 6 Widgets, the viewport on Qt's RHI). Off by default: the plugin,
# the CLI and the tests build without Qt. Needs Qt 6.8 (docs/DEVELOPING.md, "Standalone app").
option(GROOMLAB_BUILD_APP     "Build the standalone GroomLab app (needs Qt 6.8)"  OFF)
set(GROOMLAB_QT_DIR "" CACHE PATH "Qt 6.8 kit root for the app, e.g. .../Qt/6.8.3/msvc2022_64 (else CMAKE_PREFIX_PATH)")
option(GROOMLAB_COVERAGE      "Instrument the core and tests for gcov (GCC/Clang)" OFF)
# Research prototypes (tools/research, docs/research): benchmarks of ideas, never run by ctest.
option(GROOMLAB_BUILD_RESEARCH "Build the research prototypes (tools/research)"   OFF)
# The core in the browser (engine/web, docs/WEB.md): with Emscripten (the wasm preset) the
# WebAssembly module the website's /view page runs; natively its probe (groomlab_web_probe), the
# same binding for comparing the two builds' strands.
option(GROOMLAB_BUILD_WEB     "Build the WebAssembly module (Emscripten) or its native probe" OFF)

set(MAYA_LOCATION "$ENV{MAYA_LOCATION}" CACHE PATH "Maya install root, e.g. C:/Program Files/Autodesk/Maya2022")
set(MTOA_LOCATION "$ENV{MTOA_LOCATION}" CACHE PATH "MtoA install root, e.g. C:/Program Files/Autodesk/Arnold/maya2022")
if(APPLE)
    # macOS: MAYA_LOCATION is the Maya devkit (headers); the libraries the plugin links (OpenMaya...,
    # TBB) and mayapy are the install's, inside Maya.app (docs/MACOS.md).
    set(GROOMLAB_MAYA_APP "" CACHE PATH "macOS: the Maya install's Maya.app/Contents (MacOS/: its libraries, bin/mayapy), e.g. /Applications/Autodesk/maya2025/Maya.app/Contents")
endif()
# RenderMan Pro Server: RMANTREE, else the default install of 27.3 when it is there, else none.
set(_rmantree "$ENV{RMANTREE}")
if(NOT _rmantree AND EXISTS "C:/Program Files/Pixar/RenderManProServer-27.3/include/ri.h")
    set(_rmantree "C:/Program Files/Pixar/RenderManProServer-27.3")
endif()
if(NOT _rmantree AND APPLE)
    # macOS: Pixar's default install (docs/MACOS.md).
    if(EXISTS "/Applications/Pixar/RenderManProServer-27.3/include/ri.h")
        set(_rmantree "/Applications/Pixar/RenderManProServer-27.3")
    endif()
elseif(NOT _rmantree AND NOT WIN32)
    # Linux: where docs/DEVELOPING.md ("Linux builds") says to unpack it, else Pixar's default install.
    foreach(_root "$ENV{HOME}/groomlab-deps/sdks/renderman-linux" "/opt/pixar/RenderManProServer-27.3")
        if(EXISTS "${_root}/include/ri.h")
            set(_rmantree "${_root}")
            break()
        endif()
    endforeach()
endif()
set(GROOMLAB_RMANTREE "${_rmantree}" CACHE PATH "RenderMan Pro Server root (RMANTREE) for the RenderMan procedural; empty: none")
# A standalone Arnold SDK for the Arnold procedural, for builds without MtoA (Linux farms):
# ARNOLD_SDK, else ~/groomlab-deps/sdks/arnold-linux on Linux when it is there. Empty: MtoA's.
set(_arnold_sdk "$ENV{ARNOLD_SDK}")
if(NOT _arnold_sdk AND NOT WIN32 AND EXISTS "$ENV{HOME}/groomlab-deps/sdks/arnold-linux/include/ai.h")
    set(_arnold_sdk "$ENV{HOME}/groomlab-deps/sdks/arnold-linux")
endif()
if(NOT _arnold_sdk AND APPLE AND EXISTS "$ENV{HOME}/groomlab-deps/sdks/arnold-macos/include/ai.h")
    set(_arnold_sdk "$ENV{HOME}/groomlab-deps/sdks/arnold-macos")  # (docs/MACOS.md)
endif()
set(GROOMLAB_ARNOLD_SDK "${_arnold_sdk}" CACHE PATH "Arnold SDK root (include/ai.h, bin/libai.so) for the Arnold procedural without MtoA; empty: MtoA's")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GroomLabBuildSpeed)

if(MSVC)
    add_compile_options(/utf-8)
    # Release builds (RelWithDebInfo, every product) inline any function the compiler finds worth
    # it (/Ob2), not only those marked inline (CMake's /Ob1): with /Ob1, small helpers such as
    # std::floor, std::clamp and atomic loads stayed calls inside the hottest loops. Inlining
    # changes no float result under /fp:precise (test_determinism.cpp checks).
    foreach(_lang C CXX)
        string(REPLACE "/Ob1" "/Ob2" CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}")
    endforeach()
endif()
if(EMSCRIPTEN)
    # The browser build (engine/web): C++ exceptions as WebAssembly exceptions (the core throws
    # GraphError and the like; every browser since 2022 runs them), in every object and the link.
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

# Determinism: a groom evaluates to the same bits in every host built here, so the float math is
# compiled as written. MSVC: its default /fp:precise (on x64 without /arch it emits no FMA, and
# VS 2022 contracts nothing under it without /fp:contract). GCC and Clang: no contraction of
# a * b + c into an FMA (GCC contracts by default where the target has FMA, Clang on arm64), no
# fast math. Never add /fp:fast, /fp:contract, /arch:AVX2 (with VS 2019), -ffast-math or
# -march=native to the core: tests/engine/core/test_determinism.cpp is the check.
if(NOT MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off> $<$<COMPILE_LANGUAGE:C,CXX>:-fno-fast-math>)
endif()
# The C runtimes' float transcendental functions (sinf, expf, powf...) differ between MSVC and
# glibc in the last bit: the core evaluates them in double precision and rounds, so Windows and
# Linux make the same bits (engine/core/include/groomlab/portable_math.h has the measurements). OFF
# (the C runtime's float functions) is for comparisons only: the goldens are recorded with it on.
option(GROOMLAB_PORTABLE_MATH "Core float sin/exp/pow... in double precision: the same bits on every platform" ON)
if(NOT GROOMLAB_PORTABLE_MATH)
    # A build folder made before the default turned ON keeps OFF in its cache: say so loudly, as
    # it makes other strands than every release (and the goldens are recorded with it on).
    message(WARNING "GROOMLAB_PORTABLE_MATH is OFF: this build's strands differ from releases' in the last bits. "
                    "Reconfigure with -DGROOMLAB_PORTABLE_MATH=ON unless you are comparing math.")
endif()

# Strict warnings for GroomLab's own targets only; third-party code fetched
# into the build keeps its own settings.
add_library(groomlab_warnings INTERFACE)
add_library(groomlab::warnings ALIAS groomlab_warnings)
if(MSVC)
    target_compile_options(groomlab_warnings INTERFACE /W4 /permissive-)
else()
    # Aggregates initialized with fewer values than members ({a, b} value-initializes the rest, as
    # the code means) are no warning in MSVC's /W4 either.
    target_compile_options(groomlab_warnings INTERFACE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# Line coverage of the core (spec: at least 80%), reported by gcovr in CI.
if(GROOMLAB_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "GROOMLAB_COVERAGE needs GCC or Clang")
    endif()
    target_compile_options(groomlab_warnings INTERFACE --coverage -O0)
    target_link_options(groomlab_warnings INTERFACE --coverage)
endif()

include(GroomLabTBB)
include(GroomLabDependencies)

# GroomLab's license (Apache-2.0), its NOTICE and the third-party notices: every package ships them.
function(groomlab_install_legal destination component)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            DESTINATION "${destination}" COMPONENT ${component})
endfunction()

if(GROOMLAB_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine/core)

# The GPU look preview's engine (Windows: DirectX 11), host-free beside the core.
add_subdirectory(engine/gpu)

if(GROOMLAB_BUILD_ALEMBIC)
    add_subdirectory(engine/io)
endif()

if(GROOMLAB_BUILD_PYTHON)
    add_subdirectory(engine/python)
endif()

if(GROOMLAB_BUILD_CLI)
    add_subdirectory(engine/cli)
endif()

# groomlab-helper: the core in a process of its own for the Blender add-on (docs/BLENDER.md), and
# the add-on (GPL) beside it: together the GroomLab Blender extension (install component Blender).
if(GROOMLAB_BUILD_BLENDER_HELPER)
    add_subdirectory(hosts/blender/helper)
    add_subdirectory(hosts/blender/addon)
endif()

# The stable C API for host bridges (groomlab_c.h) and its C++ wrapper (groomlab.hpp).
if(GROOMLAB_BUILD_CAPI)
    add_subdirectory(engine/capi)
endif()

if(GROOMLAB_BUILD_MAYA)
    add_subdirectory(hosts/maya)
endif()

# The Arnold procedural needs the Arnold SDK (under MTOA_LOCATION or GROOMLAB_ARNOLD_SDK) and
# Alembic, not Maya.
if(GROOMLAB_BUILD_ARNOLD AND GROOMLAB_BUILD_PROCEDURAL AND TARGET groomlab_alembic)
    find_package(MtoA QUIET)
    if(TARGET Arnold::Arnold)
        add_subdirectory(hosts/arnold)
    elseif(NOT WIN32)
        message(STATUS "No Arnold SDK (GROOMLAB_ARNOLD_SDK, MTOA_LOCATION): the Arnold procedural is left out")
    endif()
endif()

# The RenderMan procedural needs RenderMan Pro Server's SDK (GROOMLAB_RMANTREE) and Alembic, not Maya.
if(GROOMLAB_BUILD_RENDERMAN AND GROOMLAB_RMANTREE AND TARGET groomlab_alembic)
    find_package(RenderMan QUIET)
    if(TARGET RenderMan::RenderMan)
        message(STATUS "RenderMan ${RenderMan_VERSION} SDK: ${GROOMLAB_RMANTREE} (building groomlab_renderman)")
        add_subdirectory(hosts/renderman)
    else()
        message(STATUS "No RenderMan SDK under ${GROOMLAB_RMANTREE}: the RenderMan procedural is left out")
    endif()
endif()

if(GROOMLAB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(GROOMLAB_BUILD_RESEARCH)
    add_subdirectory(tools/research)
endif()

if(GROOMLAB_BUILD_WEB)
    add_subdirectory(engine/web)
endif()

# After the tests: the app's own tests (tests/hosts/app) use their GoogleTest.
if(GROOMLAB_BUILD_APP)
    add_subdirectory(hosts/app)
endif()
