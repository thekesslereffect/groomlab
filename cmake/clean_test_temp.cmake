# SPDX-License-Identifier: Apache-2.0
# Empties the tests' temp folder (GroomLabTestTemp.cmake): cmake -DDIR=<folder> -P this.
# A file something still holds open is left for the next run rather than failing this one. Never a
# drive or a folder right under one: a mistyped GROOMLAB_TEST_TEMP must not empty anything else.
file(TO_CMAKE_PATH "${DIR}" DIR)
if(NOT DIR MATCHES "^(/|[A-Za-z]:/)[^/]+/[^/]+/.+[^/]$")
    message(FATAL_ERROR "not a folder for the tests' temp files: '${DIR}'")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -rf "${DIR}" OUTPUT_QUIET ERROR_QUIET)
file(MAKE_DIRECTORY "${DIR}")
