# SPDX-License-Identifier: GPL-3.0-or-later
# GroomLab's Blender add-on (GPL-3.0-or-later, this folder): Python that drives groomlab-helper,
# installed beside the helper's bin/ so that GroomLabBlender/ is a Blender extension folder
# (blender_manifest.toml at its root). docs/BLENDER.md.

if(WIN32)
    set(GROOMLAB_BLENDER_PLATFORM "windows-x64")
elseif(APPLE)
    set(GROOMLAB_BLENDER_PLATFORM "macos-arm64")
else()
    set(GROOMLAB_BLENDER_PLATFORM "linux-x64")
endif()
set(GROOMLAB_BLENDER_PLATFORM "${GROOMLAB_BLENDER_PLATFORM}" PARENT_SCOPE)

configure_file(blender_manifest.toml.in "${CMAKE_CURRENT_BINARY_DIR}/blender_manifest.toml" @ONLY)
file(GLOB _addon_py CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/groomlab/*.py")
install(FILES ${_addon_py} "${CMAKE_CURRENT_BINARY_DIR}/blender_manifest.toml" "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "GroomLabBlender" COMPONENT Blender)
# The palette's icons (Maya's, rendered to PNG by tools/make_blender_icons.py) for bpy.utils.previews.
file(GLOB _addon_icons CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/groomlab/icons/*.png")
install(FILES ${_addon_icons} DESTINATION "GroomLabBlender/icons" COMPONENT Blender)
# The UI structure every host shares (engine/ui/*.json, Apache-2.0 data): plain files the add-on reads
# without the engine (docs/UI_DEFINITIONS.md).
file(GLOB _addon_ui CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/engine/ui/*.json")
install(FILES ${_addon_ui} DESTINATION "GroomLabBlender/ui" COMPONENT Blender)

# The extension assembled in the build tree (the Blender component installed into
# build/blender_extension/<config>/GroomLabBlender) and zipped as it ships:
# GroomLab-Blender-<version>-<platform>.zip. The tests install that folder into a throwaway Blender
# profile; tools/package_blender.py makes the release zip the same way.
set(_ext_root "${CMAKE_BINARY_DIR}/blender_extension/$<CONFIG>")
set(_ext_zip "${_ext_root}/GroomLab-Blender-${PROJECT_VERSION}-${GROOMLAB_BLENDER_PLATFORM}.zip")
file(GLOB_RECURSE _addon_presets CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/hosts/maya/presets/*" "${PROJECT_SOURCE_DIR}/hosts/maya/nodes/*"
     "${PROJECT_SOURCE_DIR}/hosts/maya/meshes/*" "${PROJECT_SOURCE_DIR}/hosts/maya/alphas/*")
add_custom_command(
    OUTPUT "${_ext_root}/assembled.stamp"
    COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG> -DSTAGE=${_ext_root}
            -DZIP=${_ext_zip} -P "${CMAKE_CURRENT_SOURCE_DIR}/assemble_extension.cmake"
    DEPENDS groomlab-helper ${_addon_py} ${_addon_icons} "${CMAKE_CURRENT_BINARY_DIR}/blender_manifest.toml" LICENSE
            assemble_extension.cmake ${_addon_presets} ${_addon_ui}
    COMMENT "Assembling the GroomLab Blender extension"
    VERBATIM)
add_custom_target(groomlab_blender_extension ALL DEPENDS "${_ext_root}/assembled.stamp")
set(GROOMLAB_BLENDER_EXTENSION_ROOT "${_ext_root}" PARENT_SCOPE)
set(GROOMLAB_BLENDER_EXTENSION_ZIP "${_ext_zip}" PARENT_SCOPE)
