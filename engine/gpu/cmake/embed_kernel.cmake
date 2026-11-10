# SPDX-License-Identifier: Apache-2.0
# Build step (cmake -P): one kernel of shaders/look.slang compiled by slangc for one target, embedded
# in a header as a byte array (MSVC's string literals stop at 16 KB).
#   -DSLANGC=<slangc.exe> -DSOURCE=<look.slang> -DENTRY=<entry point> -DFORMAT=dxbc|glsl|metal
#   -DOUT=<compiled file> -DHEADER=<out .h> [-DCACHE=<folder>]
# dxbc: DirectX 11 bytecode (cs_5_0; Slang hands its HLSL to Windows' d3dcompiler_47), as
# `const unsigned char kLookDxbc<entry>[]`. glsl: GLSL 4.50 compute text for the OpenGL driver, as
# `const char kLookGlsl<entry>[]`, NUL-terminated. metal: Metal Shading Language text for the Metal
# engine (macOS: compiled by the system's Metal compiler at runtime, which caches it), as
# `const char kLookMetal<entry>[]`, NUL-terminated, and its threadgroup's size (Metal's dispatch
# names it: Slang's reflection) as `const unsigned kLookMetal<entry>Group[3]`. Its buffers have
# Slang's indices, the same in every kernel (the resources' order in look.slang).
#
# d3dcompiler is the slow part (ClearanceCS: minutes; slangc's HLSL for it: about a second), and every
# kernel of a source rebuilds when the source changes. So a dxbc kernel is keyed by the HLSL slangc
# makes for that one entry point: CACHE (shared by every build folder on the machine) holds the
# bytecode by that key, and an edit that leaves a kernel's HLSL alone reuses it. The header is only
# rewritten when its bytes change, so the C++ that embeds it does not rebuild either.
if(FORMAT STREQUAL "dxbc")
    set(profile -profile cs_5_0)
    set(name "kLookDxbc${ENTRY}")
    set(type "unsigned char")
    set(tail "")
elseif(FORMAT STREQUAL "glsl")
    set(profile -profile glsl_450 -DGROOMLAB_SLANG_GL)  # (the image's binding: OpenGL's image units are few)
    set(name "kLookGlsl${ENTRY}")
    set(type "char")
    set(tail "0x00")
elseif(FORMAT STREQUAL "metal")
    set(profile -DGROOMLAB_SLANG_LAYOUT -DGROOMLAB_SLANG_METAL -reflection-json "${OUT}.json")  # one buffer table: Slang lays it out
    set(name "kLookMetal${ENTRY}")
    set(type "char")
    set(tail "0x00")
else()
    message(FATAL_ERROR "embed_kernel.cmake: unknown format ${FORMAT}")
endif()
set(flags -entry ${ENTRY} ${profile} -O3 -line-directive-mode none -warnings-as-errors all)

set(cached "")
if(FORMAT STREQUAL "dxbc" AND CACHE)
    execute_process(
        COMMAND "${SLANGC}" "${SOURCE}" ${flags} -target hlsl -o "${OUT}.hlsl"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "slangc ${ENTRY} (hlsl): ${output}${errors}")
    endif()
    file(READ "${OUT}.hlsl" hlsl)
    string(SHA256 key "1|${SLANGC}|${flags}|${hlsl}")  # (bump the leading 1 if the bytecode's making changes)
    set(cached "${CACHE}/${ENTRY}-${key}.dxbc")
endif()

if(cached AND EXISTS "${cached}")
    file(COPY_FILE "${cached}" "${OUT}")
else()
    execute_process(
        COMMAND "${SLANGC}" "${SOURCE}" ${flags} -target ${FORMAT} -o "${OUT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "slangc ${ENTRY} (${FORMAT}): ${output}${errors}")
    endif()
    if(cached)
        # Whole, then renamed into place: a build folder compiling the same kernel at the same time
        # never reads half a file.
        file(MAKE_DIRECTORY "${CACHE}")
        string(RANDOM LENGTH 8 tag)
        file(COPY_FILE "${OUT}" "${cached}.tmp-${tag}")
        file(RENAME "${cached}.tmp-${tag}" "${cached}" RESULT renamed)
        file(REMOVE "${cached}.tmp-${tag}")
    endif()
endif()

if(FORMAT STREQUAL "glsl")
    # The app's point texture is declared in descriptor set 1 for OpenGL (look.slang: Slang
    # checks bindings for overlaps across every kind of resource, and image units are few), but
    # OpenGL's GLSL knows no sets: the qualifier goes, the binding (image unit 0) stays.
    file(READ "${OUT}" glsl)
    string(REPLACE "layout(binding = 0, set = 1)" "layout(binding = 0)" glsl "${glsl}")
    file(WRITE "${OUT}" "${glsl}")
endif()
file(READ "${OUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
if(tail STREQUAL "")
    string(REGEX REPLACE ",$" "" bytes "${bytes}")
    string(REGEX REPLACE ",\n$" "\n" bytes "${bytes}")
endif()
set(extra "")
if(FORMAT STREQUAL "metal")
    file(READ "${OUT}.json" reflection)
    string(JSON group GET "${reflection}" entryPoints 0 threadGroupSize)
    string(JSON gx GET "${group}" 0)
    string(JSON gy GET "${group}" 1)
    string(JSON gz GET "${group}" 2)
    set(extra "static const unsigned ${name}Group[3] = {${gx}, ${gy}, ${gz}};\n")
endif()
file(WRITE "${HEADER}.tmp" "// Generated from engine/gpu/shaders/look.slang (${ENTRY}, ${FORMAT}) by slangc.\nstatic const ${type} ${name}[] = {\n${bytes}${tail}};\n${extra}")
file(COPY_FILE "${HEADER}.tmp" "${HEADER}" ONLY_IF_DIFFERENT)
file(REMOVE "${HEADER}.tmp")
