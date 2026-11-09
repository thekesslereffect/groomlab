# SPDX-License-Identifier: Apache-2.0
# Finds the MtoA (Arnold for Maya) extension SDK under MTOA_LOCATION, and the Arnold SDK: MtoA's
# copy of it, or a standalone Arnold SDK under GROOMLAB_ARNOLD_SDK (what a Linux farm build uses:
# Arnold-7.x.y.z-linux.tgz unpacked, include/ai.h and bin/libai.so).
#
# Defines:
#   MtoA_FOUND, MtoA_VERSION, MtoA_INCLUDE_DIR, ARNOLD_INCLUDE_DIR
#   MtoA::MtoA       interface target: MtoA API + Arnold core headers and libs
#   Arnold::Arnold   interface target: Arnold core headers and libs only

find_path(MtoA_INCLUDE_DIR extension/Extension.h
    HINTS "${MTOA_LOCATION}"
    PATH_SUFFIXES include/mtoa
    NO_DEFAULT_PATH)

# MtoA: include/arnold/ai.h. The Arnold SDK: include/ai.h.
find_path(ARNOLD_INCLUDE_DIR ai.h
    HINTS ${GROOMLAB_ARNOLD_SDK} "${MTOA_LOCATION}"
    PATH_SUFFIXES include/arnold include
    NO_DEFAULT_PATH)

find_library(MtoA_API_LIBRARY NAMES mtoa_api HINTS "${MTOA_LOCATION}" PATH_SUFFIXES lib bin NO_DEFAULT_PATH)
# ai.lib in lib/ on Windows; libai.so in bin/ on Linux (MtoA's and the SDK's alike).
find_library(ARNOLD_LIBRARY NAMES ai HINTS ${GROOMLAB_ARNOLD_SDK} "${MTOA_LOCATION}" PATH_SUFFIXES lib bin NO_DEFAULT_PATH)

if(MtoA_INCLUDE_DIR AND EXISTS "${MtoA_INCLUDE_DIR}/utils/Version.h")
    file(STRINGS "${MtoA_INCLUDE_DIR}/utils/Version.h" _mtoa_ver REGEX "#define MTOA_(ARCH|MAJOR|MINOR)_VERSION_NUM [0-9]+")
    string(REGEX REPLACE ".*ARCH_VERSION_NUM ([0-9]+).*" "\\1" _a "${_mtoa_ver}")
    string(REGEX REPLACE ".*MAJOR_VERSION_NUM ([0-9]+).*" "\\1" _b "${_mtoa_ver}")
    string(REGEX REPLACE ".*MINOR_VERSION_NUM ([0-9]+).*" "\\1" _c "${_mtoa_ver}")
    set(MtoA_VERSION "${_a}.${_b}.${_c}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MtoA
    REQUIRED_VARS MtoA_INCLUDE_DIR ARNOLD_INCLUDE_DIR MtoA_API_LIBRARY ARNOLD_LIBRARY
    VERSION_VAR MtoA_VERSION)

# The Arnold SDK alone (headers + ai library), for the render-time procedural.
if(ARNOLD_INCLUDE_DIR AND ARNOLD_LIBRARY AND NOT TARGET Arnold::Arnold)
    add_library(Arnold::Arnold INTERFACE IMPORTED)
    target_include_directories(Arnold::Arnold INTERFACE "${ARNOLD_INCLUDE_DIR}")
    target_link_libraries(Arnold::Arnold INTERFACE "${ARNOLD_LIBRARY}")
endif()

if(MtoA_FOUND AND NOT TARGET MtoA::MtoA)
    add_library(MtoA::MtoA INTERFACE IMPORTED)
    target_include_directories(MtoA::MtoA INTERFACE "${MtoA_INCLUDE_DIR}" "${ARNOLD_INCLUDE_DIR}")
    target_link_libraries(MtoA::MtoA INTERFACE "${MtoA_API_LIBRARY}" "${ARNOLD_LIBRARY}")
    if(APPLE)
        # MtoA's platform/Platform.h defines DLLEXPORT, LIBEXT and its Event class by _DARWIN.
        target_compile_definitions(MtoA::MtoA INTERFACE _DARWIN)
    endif()
endif()
