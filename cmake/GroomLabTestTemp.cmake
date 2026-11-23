# SPDX-License-Identifier: Apache-2.0
# The tests' temp folder: one folder of the build's own inside the user's temp folder
# (<temp>/groomlab_tests/<build id>), not the temp folder itself. The suites make thousands of
# folders a run (tempfile.mkdtemp, the hosts' own untitled-scene folders, Maya's and Blender's
# files) and most never remove them, so the user's temp folder grew by gigabytes a day. ctest
# empties the folder before the first test that uses it and after the last one (a fixture: under a
# label or name filter too), so one run's files are all that is ever there.
#
# Not under the build: the updater's tests make a fake install in the temp folder, and an install
# inside a source or build tree is a developer build that never updates.
#
# One folder a build, shared by its tests as the user's temp folder was. Two ctest runs of the same
# build at once would empty each other's files: run them one after the other.
if(WIN32)
    set(_groomlab_system_temp "$ENV{TEMP}")
else()
    set(_groomlab_system_temp "$ENV{TMPDIR}")
endif()
if(NOT _groomlab_system_temp)
    set(_groomlab_system_temp "/tmp")
endif()
file(TO_CMAKE_PATH "${_groomlab_system_temp}" _groomlab_system_temp)
string(MD5 _groomlab_build_id "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_groomlab_build_id}" 0 8 _groomlab_build_id)
set(GROOMLAB_TEST_TEMP "${_groomlab_system_temp}/groomlab_tests/${_groomlab_build_id}" CACHE PATH
    "The tests' temp folder: emptied before and after every ctest run")

# Call at the end of a directory that adds tests: every test of the directory (and the discovered
# gtest lists named in ARGN, gtest_discover_tests' TEST_LIST) runs with TEMP, TMP and TMPDIR set to
# the folder. Not the look-over tools (labels `sheet`, `bench`): their pictures and numbers go to
# the temp folder by default and are meant to be read after the run.
function(groomlab_tests_use_run_temp)
    file(TO_NATIVE_PATH "${GROOMLAB_TEST_TEMP}" temp)
    set(env "TEMP=set:${temp}" "TMP=set:${temp}" "TMPDIR=set:${temp}")
    set(clean "${CMAKE_COMMAND}" "-DDIR=${GROOMLAB_TEST_TEMP}" -P "${PROJECT_SOURCE_DIR}/cmake/clean_test_temp.cmake")
    get_property(added GLOBAL PROPERTY GROOMLAB_TEST_TEMP_FIXTURE)
    if(NOT added)
        set_property(GLOBAL PROPERTY GROOMLAB_TEST_TEMP_FIXTURE TRUE)
        file(MAKE_DIRECTORY "${GROOMLAB_TEST_TEMP}")
        add_test(NAME temp.empty_before COMMAND ${clean})
        add_test(NAME temp.empty_after COMMAND ${clean})
        set_tests_properties(temp.empty_before PROPERTIES FIXTURES_SETUP groomlab_test_temp)
        set_tests_properties(temp.empty_after PROPERTIES FIXTURES_CLEANUP groomlab_test_temp)
    endif()
    get_property(tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS tests)
        get_property(labels TEST ${test} PROPERTY LABELS)
        if(test MATCHES "^temp\\.empty_" OR "sheet" IN_LIST labels OR "bench" IN_LIST labels)
            continue()
        endif()
        set_property(TEST ${test} APPEND PROPERTY FIXTURES_REQUIRED groomlab_test_temp)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT_MODIFICATION ${env})
    endforeach()
    # Discovered tests exist only when ctest runs: a file it includes after the discovery's.
    foreach(test_list IN LISTS ARGN)
        set(file "${CMAKE_CURRENT_BINARY_DIR}/${test_list}_temp.cmake")
        list(JOIN env ";" joined)
        string(REPLACE "\\" "\\\\" joined "${joined}")
        file(WRITE "${file}" "if(${test_list})\n  set_tests_properties(\${${test_list}} PROPERTIES FIXTURES_REQUIRED groomlab_test_temp ENVIRONMENT_MODIFICATION \"${joined}\")\nendif()\n")
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${file}")
    endforeach()
endfunction()
