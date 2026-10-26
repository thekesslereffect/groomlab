# SPDX-License-Identifier: Apache-2.0
# groomlab_gpu: the GPU look preview's engine (include/groomlab/gpu/look_chain.h, look_engine.h,
# look_preview.h).
# The graph's last nodes (Interpolate, Length, Clump, Noise, Curl, Width) as compute kernels for a
# viewport while a param is dragged or guides are brushed. Host-free (the
# core and the GPU API only): Maya's plugin drives it on Viewport 2.0's device, the tests on one of
# their own. Windows only for now: DirectX 11 and OpenGL 4.5 (through the context current on the
# caller's thread, functions from wglGetProcAddress).
#
# The kernels are one Slang source, shaders/look.slang, compiled when the module builds by Slang's
# slangc, a build tool only: the release below, fetched at configure time and pinned by hash like
# the other dependencies (or GROOMLAB_SLANGC: a slangc of your own, to configure offline). Nothing
# of Slang ships or runs in Maya. The plugin embeds, per kernel:
#   DirectX 11: DXBC (cs_5_0; slangc hands its HLSL to Windows' own d3dcompiler_47);
#   OpenGL: GLSL 4.50 compute text, compiled by the driver (its shader cache keeps it).
# SPIR-V (Vulkan) and Metal are compiled by a test (gpu.kernel_targets) and not embedded yet.
if(NOT WIN32)
    # (Its OpenGL backend takes its functions from WGL and its kernels' DXBC from d3dcompiler: a
    # Linux backend, GLX/EGL or Vulkan, and macOS's Metal (docs/MACOS.md) are to come. The CPU
    # operators it previews are the core's.)
    if(NOT (GROOMLAB_BUILD_MAYA OR GROOMLAB_BUILD_APP))
        message(STATUS "GPU look preview (engine/gpu): Windows only for now, left out with its tests (groomlab_gpu_tests)")
        return()
    endif()
    # The hosts compile against the engine (the Maya plugin, the app): its planning, run algorithm
    # and LineFrames' shared part build without a backend (src/look_engine_none.cpp: every factory
    # throws, so the hosts preview on the CPU). No slangc, no kernels embedded, no device tests.
    add_library(groomlab_gpu STATIC
        src/look_chain.cpp
        src/look_engine.cpp
        src/look_engine_internal.h
        src/look_engine_none.cpp
        src/look_preview.cpp
        src/line_frames.cpp)
    target_include_directories(groomlab_gpu PUBLIC include)
    target_link_libraries(groomlab_gpu PUBLIC groomlab::core PRIVATE groomlab::warnings)
    add_library(groomlab::gpu ALIAS groomlab_gpu)
    message(STATUS "GPU look preview (engine/gpu): no backend on this platform yet, the hosts preview on the CPU")
    # The kernels still compile for SPIR-V and Metal with a slangc of this platform's (Slang's
    # macos-aarch64 or linux-x86_64 release): GROOMLAB_SLANGC.
    set(GROOMLAB_SLANGC "" CACHE FILEPATH "slangc for the gpu.*kernel_targets tests (empty: they are left out)")
    if(GROOMLAB_BUILD_TESTS AND GROOMLAB_SLANGC)
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/groomlab/gpu/look_kernels.inc" _kernel_lines REGEX "^GROOMLAB_KERNEL\\(")
        set(_kernels "")
        foreach(_line IN LISTS _kernel_lines)
            string(REGEX REPLACE "^GROOMLAB_KERNEL\\(([A-Za-z0-9_]+)\\).*" "\\1CS" _k "${_line}")
            list(APPEND _kernels "${_k}")
        endforeach()
        set(_checks_kernel_targets look.slang ${_kernels})
        set(_checks_line_kernel_targets lines.slang FramesCS WidthsCS RibbonsCS RibbonLayoutCS RibbonColorsCS)
        set(_checks_follow_kernel_targets follow.slang FollowCS)
        foreach(_name kernel_targets line_kernel_targets follow_kernel_targets)
            set(_entries ${_checks_${_name}})
            list(POP_FRONT _entries _file)
            add_test(NAME gpu.${_name}
                COMMAND "${CMAKE_COMMAND}" -DSLANGC=${GROOMLAB_SLANGC} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_file}
                        "-DENTRIES=${_entries}" -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${_name}
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_kernel_targets.cmake")
            set_tests_properties(gpu.${_name} PROPERTIES LABELS "quick;gpu")
        endforeach()
    endif()
    return()
endif()

set(GROOMLAB_SLANGC "" CACHE FILEPATH "slangc.exe for the GPU preview's kernels (empty: Slang 2026.19 is fetched)")
set(_slangc "${GROOMLAB_SLANGC}")
if(NOT _slangc)
    include(FetchContent)
    FetchContent_Declare(slang
        URL https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-windows-x86_64.zip
        URL_HASH SHA256=FC922F214B5D115F663932E59E80E0F6B6CC6FAE140979CF307DA6459B6F8F33
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-project)  # binaries only: nothing to add to the build
    groomlab_cached_source(slang 2026.19)
    FetchContent_MakeAvailable(slang)
    groomlab_store_source(slang)
    set(_slangc "${slang_SOURCE_DIR}/bin/slangc.exe")
endif()
message(STATUS "GPU preview kernels: ${_slangc}")

# The compiled DirectX kernels, shared by every build folder on the machine and keyed by each
# kernel's HLSL (cmake/embed_kernel.cmake): d3dcompiler takes minutes over ClearanceCS, so an edit
# recompiles only the kernels it changes, and each Maya version's tree reuses the others'. Empty:
# every kernel compiles each time its source changes.
if(WIN32)
    set(_kernel_cache_default "$ENV{USERPROFILE}/groomlab-deps/kernels")
else()
    set(_kernel_cache_default "")
endif()
set(GROOMLAB_KERNEL_CACHE "${_kernel_cache_default}" CACHE PATH
    "Shared folder of compiled GPU kernels, keyed by their HLSL (empty: no cache)")
set(_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/look.slang")
# The math look.slang shares with the core (it includes it): an edit there recompiles every kernel.
set(_shared_math "${CMAKE_CURRENT_SOURCE_DIR}/../core/include/groomlab/shared/groom_math.h")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Its kernels: one a line of include/groomlab/gpu/look_kernels.inc, which makes the engine's Kernel
# enum and the backends' tables too ("GROOMLAB_KERNEL(Name)": the entry point NameCS).
set(_kernel_list "${CMAKE_CURRENT_SOURCE_DIR}/include/groomlab/gpu/look_kernels.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kernel_list}")
file(STRINGS "${_kernel_list}" _kernel_lines REGEX "^GROOMLAB_KERNEL\\(")
set(_kernels "")
foreach(_line IN LISTS _kernel_lines)
    string(REGEX REPLACE "^GROOMLAB_KERNEL\\(([A-Za-z0-9_]+)\\).*" "\\1CS" _k "${_line}")
    list(APPEND _kernels "${_k}")
endforeach()
set(_kernel_headers "")
foreach(_target dxbc glsl)
    set(_includes "")
    foreach(_k IN LISTS _kernels)
        set(_h "${_gen}/look_${_k}_${_target}.h")
        add_custom_command(OUTPUT "${_h}"
            COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_source} -DENTRY=${_k} -DFORMAT=${_target} -DCACHE=${GROOMLAB_KERNEL_CACHE}
                    -DOUT=${_gen}/look_${_k}.${_target} -DHEADER=${_h} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            DEPENDS "${_source}" "${_shared_math}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            COMMENT "slangc: ${_k} (${_target})"
            VERBATIM)
        list(APPEND _kernel_headers "${_h}")
        string(APPEND _includes "#include \"look_${_k}_${_target}.h\"\n")
    endforeach()
    file(CONFIGURE OUTPUT "${_gen}/look_kernels_${_target}.h" CONTENT
        "#pragma once\n// Generated by engine/gpu/CMakeLists.txt: each kernel of shaders/look.slang (${_target}).\n@_includes@"
        @ONLY)
endforeach()
# The lit strand lines' frames from points already on the GPU (line_frames.h): shaders/lines.slang,
# which includes look.slang (its lineFrame).
set(_lines_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lines.slang")
set(_line_kernels FramesCS WidthsCS RibbonsCS RibbonLayoutCS RibbonColorsCS)
foreach(_target dxbc glsl)
    set(_includes "")
    foreach(_k IN LISTS _line_kernels)
        set(_h "${_gen}/lines_${_k}_${_target}.h")
        add_custom_command(OUTPUT "${_h}"
            COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_lines_source} -DENTRY=${_k} -DFORMAT=${_target} -DCACHE=${GROOMLAB_KERNEL_CACHE}
                    -DOUT=${_gen}/lines_${_k}.${_target} -DHEADER=${_h} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            DEPENDS "${_lines_source}" "${_source}" "${_shared_math}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            COMMENT "slangc: ${_k} (${_target})"
            VERBATIM)
        list(APPEND _kernel_headers "${_h}")
        string(APPEND _includes "#include \"lines_${_k}_${_target}.h\"\n")
    endforeach()
    file(CONFIGURE OUTPUT "${_gen}/line_kernels_${_target}.h" CONTENT
        "#pragma once\n// Generated by engine/gpu/CMakeLists.txt: each kernel of shaders/lines.slang (${_target}).\n@_includes@"
        @ONLY)
endforeach()

# Following the scalp on the GPU (line_frames.h runFollow): shaders/follow.slang, which includes
# lines.slang (and so look.slang).
set(_follow_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/follow.slang")
set(_follow_kernels FollowCS)
foreach(_target dxbc glsl)
    set(_includes "")
    foreach(_k IN LISTS _follow_kernels)
        set(_h "${_gen}/follow_${_k}_${_target}.h")
        add_custom_command(OUTPUT "${_h}"
            COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_follow_source} -DENTRY=${_k} -DFORMAT=${_target} -DCACHE=${GROOMLAB_KERNEL_CACHE}
                    -DOUT=${_gen}/follow_${_k}.${_target} -DHEADER=${_h} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            DEPENDS "${_follow_source}" "${_lines_source}" "${_source}" "${_shared_math}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            COMMENT "slangc: ${_k} (${_target})"
            VERBATIM)
        list(APPEND _kernel_headers "${_h}")
        string(APPEND _includes "#include \"follow_${_k}_${_target}.h\"\n")
    endforeach()
    file(CONFIGURE OUTPUT "${_gen}/follow_kernels_${_target}.h" CONTENT
        "#pragma once\n// Generated by engine/gpu/CMakeLists.txt: each kernel of shaders/follow.slang (${_target}).\n@_includes@"
        @ONLY)
endforeach()

# The hair's density grid for the viewport's self-shadowing (line_frames.h runDensity):
# shaders/density.slang, on its own.
set(_density_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/density.slang")
set(_density_kernels DensityClearCS DensitySplatCS DensityResolveCS)
foreach(_target dxbc glsl)
    set(_includes "")
    foreach(_k IN LISTS _density_kernels)
        set(_h "${_gen}/density_${_k}_${_target}.h")
        add_custom_command(OUTPUT "${_h}"
            COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_density_source} -DENTRY=${_k} -DFORMAT=${_target} -DCACHE=${GROOMLAB_KERNEL_CACHE}
                    -DOUT=${_gen}/density_${_k}.${_target} -DHEADER=${_h} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            DEPENDS "${_density_source}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake"
            COMMENT "slangc: ${_k} (${_target})"
            VERBATIM)
        list(APPEND _kernel_headers "${_h}")
        string(APPEND _includes "#include \"density_${_k}_${_target}.h\"\n")
    endforeach()
    file(CONFIGURE OUTPUT "${_gen}/density_kernels_${_target}.h" CONTENT
        "#pragma once\n// Generated by engine/gpu/CMakeLists.txt: each kernel of shaders/density.slang (${_target}).\n@_includes@"
        @ONLY)
endforeach()

# The same kernels compiled to C++ for the CPU engine (look_engine_cpu.cpp: the GPU's stages run on
# the CPU from this one source). Where a kernel's group works as one (barriers, groupshared memory),
# its C++ form runs each strand or clump whole on a thread (look.slang's __target_switch cpp cases),
# and its atomics are the core's (src/look_kernels_cpu_atomics.h, included first).
set(_cpu_kernels ${_kernels})
set(_cpu_entries "")
set(_cpu_list "")
foreach(_k IN LISTS _cpu_kernels)
    list(APPEND _cpu_entries -entry ${_k} -stage compute)
    string(REGEX REPLACE "CS$" "" _name "${_k}")
    string(APPEND _cpu_list "GROOMLAB_CPU_KERNEL(${_name})
")
endforeach()
file(CONFIGURE OUTPUT "${_gen}/look_kernels_cpu.inc" CONTENT
    "// Generated by engine/gpu/CMakeLists.txt: the kernels of shaders/look.slang the CPU engine has.
@_cpu_list@" @ONLY)
add_custom_command(OUTPUT "${_gen}/look_kernels_cpu.cpp"
    COMMAND "${_slangc}" "${_source}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" ${_cpu_entries} -target cpp
            -o "${_gen}/look_kernels_cpu.cpp"
    DEPENDS "${_source}" "${_shared_math}"
    COMMENT "slangc: the look kernels for the CPU (C++)"
    VERBATIM)
get_filename_component(_slang_root "${_slangc}" DIRECTORY)
get_filename_component(_slang_root "${_slang_root}" DIRECTORY)

add_library(groomlab_gpu STATIC
    src/look_chain.cpp
    src/look_engine.cpp
    src/look_engine_cpu.cpp
    "${_gen}/look_kernels_cpu.cpp"
    src/look_engine_internal.h
    src/look_engine_d3d11.cpp
    src/look_engine_gl.cpp
    src/look_preview.cpp
    src/line_frames.cpp
    shaders/look.slang
    shaders/lines.slang
    shaders/follow.slang
    shaders/density.slang
    ${_kernel_headers})
set_source_files_properties(shaders/look.slang shaders/lines.slang shaders/follow.slang shaders/density.slang PROPERTIES HEADER_FILE_ONLY TRUE)  # slangc builds them
target_include_directories(groomlab_gpu PUBLIC include PRIVATE "${_gen}" "${_slang_root}/include")
# (slangc's C++: its own style.)
set_source_files_properties("${_gen}/look_kernels_cpu.cpp" PROPERTIES GENERATED TRUE
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/W0;/bigobj;/FI${CMAKE_CURRENT_SOURCE_DIR}/src/look_kernels_cpu_atomics.h>")
target_link_libraries(groomlab_gpu PUBLIC groomlab::core d3d11 opengl32 PRIVATE groomlab::warnings)
add_library(groomlab::gpu ALIAS groomlab_gpu)

# The targets not embedded yet (SPIR-V, Metal) still compile from the same source.
if(GROOMLAB_BUILD_TESTS)
    add_test(NAME gpu.kernel_targets
        COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_source} "-DENTRIES=${_kernels}"
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/kernel_targets -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_kernel_targets.cmake")
    set_tests_properties(gpu.kernel_targets PROPERTIES LABELS "quick;gpu")
    add_test(NAME gpu.line_kernel_targets
        COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_lines_source} "-DENTRIES=${_line_kernels}"
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/line_kernel_targets -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_kernel_targets.cmake")
    set_tests_properties(gpu.line_kernel_targets PROPERTIES LABELS "quick;gpu")
    add_test(NAME gpu.follow_kernel_targets
        COMMAND "${CMAKE_COMMAND}" -DSLANGC=${_slangc} -DSOURCE=${_follow_source} "-DENTRIES=${_follow_kernels}"
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/follow_kernel_targets -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_kernel_targets.cmake")
    set_tests_properties(gpu.follow_kernel_targets PROPERTIES LABELS "quick;gpu")
endif()
