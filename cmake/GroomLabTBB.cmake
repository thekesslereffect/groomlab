# SPDX-License-Identifier: Apache-2.0
# Resolves TBB into the target groomlab::tbb.
#
# Order of preference:
#   1. With GROOMLAB_BUILD_MAYA: the TBB that ships inside that Maya (MAYA_LOCATION: TBB 2020 in
#      Maya 2022 to 2025, oneTBB's tbb12 in Maya 2026 and later), so the plugin links the TBB Maya
#      loads, never a system oneTBB whose DLL is not installed beside it. A Maya devkit
#      (docs/DEVELOPING.md, "Other Maya versions") has the headers and the import library but no
#      DLL: the build links it, and nothing copies a TBB runtime beside the binaries.
#   2. A TBB CMake package (oneTBB or TBB 2020 with config files), e.g. Linux libtbb-dev.
#   3. Maya's TBB when MAYA_LOCATION is set without the plugin.
#   4. None: GROOMLAB_HAS_TBB=0 and parallel loops run serially.
#
# GROOMLAB_TBB_RUNTIME is ON when groomlab::tbb has a DLL to copy beside executables and to install
# ($<TARGET_RUNTIME_DLLS:...> names it): every target that copies or installs it checks it.

set(GROOMLAB_HAS_TBB OFF)
set(GROOMLAB_TBB_RUNTIME OFF)

set(_groomlab_maya_tbb OFF)
if(MAYA_LOCATION AND EXISTS "${MAYA_LOCATION}/include/tbb/parallel_for.h")
    set(_groomlab_maya_tbb ON)
endif()

if(GROOMLAB_USE_TBB)
    if(NOT (GROOMLAB_BUILD_MAYA AND _groomlab_maya_tbb))
        find_package(TBB CONFIG QUIET)
    endif()
    if(TARGET TBB::tbb)
        add_library(groomlab::tbb ALIAS TBB::tbb)
        set(GROOMLAB_HAS_TBB ON)
        set(GROOMLAB_TBB_RUNTIME ON)
        message(STATUS "TBB: package ${TBB_VERSION}")
    elseif(_groomlab_maya_tbb AND APPLE)
        # macOS: the TBB inside the Maya install (GROOMLAB_MAYA_APP/MacOS: libtbb.dylib, oneTBB's
        # libtbb.12.dylib from Maya 2026), else a devkit's lib/. Neither: the devkit's headers alone,
        # and what links it leaves TBB's symbols to Maya's process, as FindMaya does Maya's own
        # (a Maya plugin and the Python module load into Maya, which has loaded its TBB).
        # The library of the headers' TBB: Maya 2025 ships both (oneTBB's libtbb.12.dylib beside TBB
        # 2020's libtbb.dylib) with TBB 2020's headers; oneTBB's headers (include/oneapi) from 2026.
        if(EXISTS "${MAYA_LOCATION}/include/oneapi/tbb")
            set(_groomlab_maya_tbb_names tbb.12 tbb)
        else()
            set(_groomlab_maya_tbb_names tbb)
        endif()
        find_library(GROOMLAB_MAYA_TBB_LIBRARY NAMES ${_groomlab_maya_tbb_names} HINTS "${GROOMLAB_MAYA_APP}/MacOS" "${MAYA_LOCATION}/lib"
                     NO_DEFAULT_PATH)
        if(GROOMLAB_MAYA_TBB_LIBRARY)
            # Imported as the library itself, as Linux does Maya's: the installs that carry their
            # runtime (the Arnold procedural, which kick loads without Maya) get it beside them.
            add_library(groomlab_maya_tbb SHARED IMPORTED GLOBAL)
            set_target_properties(groomlab_maya_tbb PROPERTIES IMPORTED_LOCATION "${GROOMLAB_MAYA_TBB_LIBRARY}")
            set(GROOMLAB_TBB_RUNTIME ON)
            message(STATUS "TBB: Maya bundled ${GROOMLAB_MAYA_TBB_LIBRARY}")
        else()
            add_library(groomlab_maya_tbb INTERFACE IMPORTED GLOBAL)
            target_link_options(groomlab_maya_tbb INTERFACE "LINKER:-undefined,dynamic_lookup")
            message(STATUS "TBB: Maya's headers (${MAYA_LOCATION}), its library left to Maya's process "
                           "(set GROOMLAB_MAYA_APP to link Maya.app's)")
        endif()
        target_include_directories(groomlab_maya_tbb INTERFACE "${MAYA_LOCATION}/include")
        target_compile_definitions(groomlab_maya_tbb INTERFACE TBB_USE_DEBUG=0)
        add_library(groomlab::tbb ALIAS groomlab_maya_tbb)
        set(GROOMLAB_HAS_TBB ON)
    elseif(_groomlab_maya_tbb)
        # oneTBB (Maya 2026+) names its library tbb12; TBB 2020 tbb.
        if(EXISTS "${MAYA_LOCATION}/lib/tbb12.lib" OR EXISTS "${MAYA_LOCATION}/lib/libtbb.so.12")
            set(_tbb_name tbb12)
        else()
            set(_tbb_name tbb)
        endif()
        if(WIN32)
            set(_tbb_dll "${MAYA_LOCATION}/bin/${_tbb_name}.dll")
            set(_tbb_implib "${MAYA_LOCATION}/lib/${_tbb_name}.lib")
            if(NOT EXISTS "${_tbb_dll}" AND GROOMLAB_MAYA_BIN AND EXISTS "${GROOMLAB_MAYA_BIN}/${_tbb_name}.dll")
                set(_tbb_dll "${GROOMLAB_MAYA_BIN}/${_tbb_name}.dll")  # a devkit's, run in that Maya's install
            endif()
        elseif(_tbb_name STREQUAL "tbb12")
            set(_tbb_dll "${MAYA_LOCATION}/lib/libtbb.so.12")
        else()
            set(_tbb_dll "${MAYA_LOCATION}/lib/libtbb.so.2")
        endif()
        if(EXISTS "${_tbb_dll}")
            add_library(groomlab_maya_tbb SHARED IMPORTED GLOBAL)
            if(WIN32)
                set_target_properties(groomlab_maya_tbb PROPERTIES IMPORTED_LOCATION "${_tbb_dll}" IMPORTED_IMPLIB "${_tbb_implib}")
            else()
                set_target_properties(groomlab_maya_tbb PROPERTIES IMPORTED_LOCATION "${_tbb_dll}")
            endif()
            set(GROOMLAB_TBB_RUNTIME ON)
        else()
            # A devkit: the import library alone (Maya loads the DLL).
            add_library(groomlab_maya_tbb INTERFACE IMPORTED GLOBAL)
            target_link_libraries(groomlab_maya_tbb INTERFACE "${_tbb_implib}")
        endif()
        target_include_directories(groomlab_maya_tbb INTERFACE "${MAYA_LOCATION}/include")
        # Maya ships release TBB only; stop the headers auto-linking tbb_debug.lib / tbb12_debug.lib.
        target_compile_definitions(groomlab_maya_tbb INTERFACE __TBB_NO_IMPLICIT_LINKAGE=1 TBB_USE_DEBUG=0)
        add_library(groomlab::tbb ALIAS groomlab_maya_tbb)
        set(GROOMLAB_HAS_TBB ON)
        message(STATUS "TBB: Maya bundled ${_tbb_name} (${MAYA_LOCATION})")
    else()
        message(STATUS "TBB: not found, parallel loops run serially")
    endif()
endif()

# groomlab_install_runtime(<target> <destination> <component>): Linux (ELF) binaries shipped to farms
# and pipelines find their libraries beside them ($ORIGIN in the install's RUNPATH), and the TBB
# they link is installed there under its soname (libtbb.so.12), so a render node needs nothing
# installed system-wide. macOS: the same with @loader_path and the TBB dylib under its install name's
# file name (libtbb.12.dylib). A no-op on Windows (each target copies and installs its DLLs itself:
# $<TARGET_RUNTIME_DLLS>).
function(groomlab_install_runtime target destination component)
    if(WIN32)
        return()
    endif()
    if(APPLE)
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path")
    else()
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
    if(NOT GROOMLAB_TBB_RUNTIME)
        return()
    endif()
    get_target_property(_tbb groomlab::tbb ALIASED_TARGET)
    if(NOT _tbb)
        set(_tbb groomlab::tbb)
    endif()
    set(_file "")
    # Release first; a distribution's package may name its only configuration NONE (Debian's).
    get_target_property(_configs ${_tbb} IMPORTED_CONFIGURATIONS)
    set(_props IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO)
    foreach(_config IN LISTS _configs)
        list(APPEND _props "IMPORTED_LOCATION_${_config}")
    endforeach()
    list(APPEND _props IMPORTED_LOCATION)
    foreach(_prop IN LISTS _props)
        get_target_property(_loc ${_tbb} ${_prop})
        if(_loc)
            set(_file "${_loc}")
            break()
        endif()
    endforeach()
    if(NOT _file)
        return()
    endif()
    # The soname the binaries ask for (libtbb.so.12), the real file behind it installed under it.
    get_filename_component(_name "${_file}" NAME)
    if(APPLE)
        # (macOS: libtbb.12.17.dylib, whose install name is @rpath/libtbb.12.dylib.)
        set(_soname "")
        if(_name MATCHES "^(lib[^.]+[.][0-9]+)[.].*dylib$")
            set(_soname "${CMAKE_MATCH_1}.dylib")
        endif()
    else()
        string(REGEX MATCH "^lib[^.]+\.so\.[0-9]+" _soname "${_name}")
    endif()
    if(NOT _soname)
        set(_soname "${_name}")
    endif()
    file(REAL_PATH "${_file}" _real)
    install(FILES "${_real}" DESTINATION "${destination}" RENAME "${_soname}" COMPONENT ${component})
endfunction()

# groomlab_hide_static_symbols(<target>): a module loaded into a renderer or a host (procedurals,
# the Python module) exports its entry points alone: the core, Alembic and Imath linked into it
# statically stay local, so they never interpose on a host's own copies (ELF resolves symbols
# process-wide). Windows DLLs export only what is marked dllexport already.
function(groomlab_hide_static_symbols target)
    if(WIN32 OR APPLE)
        return()
    endif()
    set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(${target} PRIVATE "LINKER:--exclude-libs,ALL")
endfunction()
