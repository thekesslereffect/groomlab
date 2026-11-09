# SPDX-License-Identifier: Apache-2.0
# Puts DIR (the build's plugin folder) on Maya's trusted plugin locations in the prefs under
# APP_DIR (the GUI tests' own MAYA_APP_DIR) before a test session starts. Maya's secure plugin
# loading asks before loading a plugin from an untrusted folder, and changing the trusted list
# while Maya runs asks again: both are dialogs nobody is there to click. Written into the prefs
# file, the entry is read at startup like any saved preference, without a question.
#
# A prefs file without Maya's version stamp (`optionVar -version 3;`) counts as a first launch:
# Maya then resets its preferences, the trusted list included (initPathList in
# createPreferencesOptVars.mel), and the plugin would ask after all. So a new prefs file starts
# with the stamp, and isFirstTimeRunningMaya 0 skips the first-run screens.
#
#   cmake -DAPP_DIR=<MAYA_APP_DIR> -DDIR=<plugin folder> -P trust_plugin_folder.cmake

if(NOT APP_DIR OR NOT DIR)
    message(FATAL_ERROR "trust_plugin_folder.cmake needs -DAPP_DIR=... and -DDIR=...")
endif()
# MAYA_YEAR: the Maya version whose prefs folder this is (<APP_DIR>/<year>/prefs); 2022 when not given.
if(NOT MAYA_YEAR)
    set(MAYA_YEAR 2022)
endif()
set(prefs "${APP_DIR}/${MAYA_YEAR}/prefs/userPrefs.mel")
set(stamp "//Maya Preference ${MAYA_YEAR} (Release 1)\n//\n//\noptionVar -version 3;\noptionVar -iv \"isFirstTimeRunningMaya\" 0;\n")
set(text "")
if(EXISTS "${prefs}")
    file(READ "${prefs}" text)
else()
    file(MAKE_DIRECTORY "${APP_DIR}/${MAYA_YEAR}/prefs")
endif()
string(FIND "${text}" "optionVar -version" stamped)
if(stamped EQUAL -1)
    set(text "${stamp}${text}")  # a new file, or one an earlier fixture wrote without the stamp
    file(WRITE "${prefs}" "${text}")
endif()
string(FIND "${text}" "\"${DIR}\"" found)
if(found GREATER -1)
    return()  # already trusted (Maya writes the list back when it saves its prefs)
endif()
file(APPEND "${prefs}" "\noptionVar -stringValueAppend \"SafeModeAllowedlistPaths\" \"${DIR}\";\n")
message(STATUS "trusted ${DIR} in ${prefs}")
