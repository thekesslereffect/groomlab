# SPDX-License-Identifier: Apache-2.0
# Builds the `groomlab_core` Python extension.
#
# With MAYA_LOCATION set, the module targets Maya's own Python (3.7 for Maya
# 2022, 3.9 for 2023, 3.10 for 2024, 3.11 for 2025 and 2026, 3.13 for 2027) so it
# imports inside Maya and mayapy. Otherwise it uses whatever Python CMake finds,
# which is enough for headless development and CI.
#
# A Maya install has the headers (include/Python3x/Python), the import library
# (lib/python3x.lib) and mayapy. A Maya devkit has the headers alone: the import
# library and the interpreter then come from GROOMLAB_PYTHON_ROOT, a CPython of
# the same minor version (the python.org NuGet package's tools/ folder, or an
# install: python.exe and libs/python3x.lib). The module links python3x.dll by
# name, which Maya's own is.
set(GROOMLAB_PYTHON_ROOT "" CACHE PATH
    "A CPython matching the Maya devkit's (python.exe, libs/python3x.lib), for a devkit without them")
set(GROOMLAB_PYBIND_INTERNALS_VERSION "" CACHE STRING
    "A unique pybind11 registry generation for loading rebuilt modules in one development process")

set(_maya_python_h "")
if(MAYA_LOCATION)
    file(GLOB _maya_python_h "${MAYA_LOCATION}/include/Python3*/Python/Python.h")
endif()
if(_maya_python_h)
    list(GET _maya_python_h 0 _maya_python_h)
    get_filename_component(_maya_python_inc "${_maya_python_h}" DIRECTORY)
    file(STRINGS "${_maya_python_inc}/patchlevel.h" _py_ver REGEX "#define PY_(MAJOR|MINOR)_VERSION[ \t]+[0-9]+")
    string(REGEX REPLACE ".*PY_MAJOR_VERSION[ \t]+([0-9]+).*" "\\1" _py_major "${_py_ver}")
    string(REGEX REPLACE ".*PY_MINOR_VERSION[ \t]+([0-9]+).*" "\\1" _py_minor "${_py_ver}")
    set(_py_xy "${_py_major}${_py_minor}")
    set(Python_INCLUDE_DIR "${_maya_python_inc}" CACHE PATH "" FORCE)
    if(EXISTS "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}")
        set(Python_EXECUTABLE "${MAYA_LOCATION}/bin/mayapy${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "" FORCE)
        if(WIN32)
            set(Python_LIBRARY "${MAYA_LOCATION}/lib/python${_py_xy}.lib" CACHE FILEPATH "" FORCE)
        elseif(APPLE)
            # (macOS: the module links no libpython: pybind11 leaves Python's symbols to the process.)
        elseif(_py_xy STREQUAL "37")
            set(Python_LIBRARY "${MAYA_LOCATION}/lib/libpython3.7m.so" CACHE FILEPATH "" FORCE)
        else()
            set(Python_LIBRARY "${MAYA_LOCATION}/lib/libpython${_py_major}.${_py_minor}.so" CACHE FILEPATH "" FORCE)
        endif()
    elseif(APPLE)
        # macOS: a module links no libpython (pybind11 leaves Python's symbols to the process that
        # imports it: -undefined dynamic_lookup), so the devkit's headers and an interpreter of their
        # minor version are all it takes: the Maya install's mayapy (GROOMLAB_MAYA_APP/bin/mayapy),
        # else GROOMLAB_PYTHON_ROOT's bin/python3.x (a python.org or Homebrew Python of that version).
        if(GROOMLAB_MAYA_APP AND EXISTS "${GROOMLAB_MAYA_APP}/bin/mayapy")
            set(Python_EXECUTABLE "${GROOMLAB_MAYA_APP}/bin/mayapy" CACHE FILEPATH "" FORCE)
        elseif(GROOMLAB_PYTHON_ROOT)
            set(Python_EXECUTABLE "${GROOMLAB_PYTHON_ROOT}/bin/python${_py_major}.${_py_minor}" CACHE FILEPATH "" FORCE)
        else()
            message(FATAL_ERROR "The Maya devkit at ${MAYA_LOCATION} has Python ${_py_major}.${_py_minor}'s headers: set "
                                "GROOMLAB_MAYA_APP (Maya.app/Contents, for its mayapy) or GROOMLAB_PYTHON_ROOT to a "
                                "Python ${_py_major}.${_py_minor} (docs/MACOS.md)")
        endif()
    else()
        if(NOT GROOMLAB_PYTHON_ROOT)
            message(FATAL_ERROR "The Maya devkit at ${MAYA_LOCATION} has Python ${_py_major}.${_py_minor}'s headers but "
                                "no interpreter or import library: set GROOMLAB_PYTHON_ROOT to a CPython "
                                "${_py_major}.${_py_minor} (docs/DEVELOPING.md, \"Other Maya versions\")")
        endif()
        set(Python_EXECUTABLE "${GROOMLAB_PYTHON_ROOT}/python${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "" FORCE)
        set(Python_LIBRARY "${GROOMLAB_PYTHON_ROOT}/libs/python${_py_xy}.lib" CACHE FILEPATH "" FORCE)
        if(NOT EXISTS "${Python_LIBRARY}")
            message(FATAL_ERROR "GROOMLAB_PYTHON_ROOT (${GROOMLAB_PYTHON_ROOT}) has no libs/python${_py_xy}.lib: "
                                "it must be a CPython ${_py_major}.${_py_minor}, the devkit's")
        endif()
    endif()
elseif(APPLE AND GROOMLAB_MAYA_APP AND EXISTS "${GROOMLAB_MAYA_APP}/bin/mayapy")
    # A macOS devkit without Python's headers: mayapy's own (the Python.framework inside Maya.app).
    set(Python_EXECUTABLE "${GROOMLAB_MAYA_APP}/bin/mayapy" CACHE FILEPATH "" FORCE)
endif()

set(PYBIND11_FINDPYTHON ON)
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
groomlab_fetch_pybind11()

pybind11_add_module(groomlab_core_py MODULE
    src/bindings.cpp
    src/bind_basic.cpp
    src/bind_strands.cpp
    src/bind_hierarchy.cpp
    src/bind_graph.cpp
    src/bind_groom.cpp
    src/bind_session.cpp
    src/bind_display.cpp
    src/bind_stroke.cpp
    src/bind_alembic.cpp
    src/bind_reference.cpp
    src/bind_card_blocks.cpp
)
set_target_properties(groomlab_core_py PROPERTIES OUTPUT_NAME groomlab_core)
target_link_libraries(groomlab_core_py PRIVATE groomlab::core groomlab::warnings)
if(GROOMLAB_PYBIND_INTERNALS_VERSION)
    target_compile_definitions(groomlab_core_py PRIVATE
        PYBIND11_INTERNALS_VERSION=${GROOMLAB_PYBIND_INTERNALS_VERSION})
endif()
if(MSVC)
    target_compile_options(groomlab_core_py PRIVATE /bigobj)
    # No incremental linking: its growing .pdb (several hundred MB with /Z7 objects) failed with
    # LNK1201 more than once; a full link of the module takes a few seconds.
    target_link_options(groomlab_core_py PRIVATE /INCREMENTAL:NO)
endif()
# Python.h (through pybind11) comes before any standard header. Every binding file includes the
# three already (src/bindings.h).
groomlab_precompile(groomlab_core_py BEFORE_STD <pybind11/pybind11.h> <pybind11/functional.h> <pybind11/stl.h>)

# Alembic and USD writers, readers and GroomScene when the Alembic library is
# built (static, so nothing extra ships next to the module).
if(TARGET groomlab_alembic)
    target_link_libraries(groomlab_core_py PRIVATE groomlab::alembic)
    target_compile_definitions(groomlab_core_py PRIVATE GROOMLAB_PY_HAS_ALEMBIC=1)
else()
    target_compile_definitions(groomlab_core_py PRIVATE GROOMLAB_PY_HAS_ALEMBIC=0)
endif()

set(GROOMLAB_PYTHON_EXECUTABLE "${Python_EXECUTABLE}" CACHE INTERNAL "Interpreter matching the groomlab_core module")
message(STATUS "groomlab_core Python: ${Python_EXECUTABLE} (${Python_VERSION})")

# Linux pipelines (a render farm's Python, outside Maya): the module and its TBB in python/
# (cmake --install <build> --component Python). The Maya module ships Windows' copy itself.
if(NOT WIN32)
    groomlab_hide_static_symbols(groomlab_core_py)
    install(TARGETS groomlab_core_py LIBRARY DESTINATION python COMPONENT Python)
    groomlab_install_legal(python Python)
    groomlab_install_runtime(groomlab_core_py python Python)
endif()

if(WIN32 AND GROOMLAB_TBB_RUNTIME)
    add_custom_command(TARGET groomlab_core_py POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:groomlab_core_py> $<TARGET_FILE_DIR:groomlab_core_py>
        COMMAND_EXPAND_LISTS)
endif()
