// SPDX-License-Identifier: Apache-2.0
// The look engine on OpenGL (look_engine.h): the same kernels as GLSL 4.50 compute (slangc's, from
// look.slang), on the context current on the caller's thread, bound by look.slang's vk::binding
// (uniform block 0 the constants, shader storage 1-3, 7-10, 12 and 13 in, 4-6 out). Maya draws
// Viewport 2.0 through OpenGL Core Profile on the main thread: its vertex buffers are plain GL
// buffers there, which the kernels bind as shader storage and write in place.
//
// No loader library: the few functions past OpenGL 1.1 come from wglGetProcAddress, the enums
// are spelled out below. Objects (buffers, programs) are shared between the contexts of a share
// group, as Maya's viewports share theirs; query objects are not, so the timestamps belong to the
// context that made them and are skipped in another.
//
// Buffers are made and written through direct state access (OpenGL 4.5): nothing of the context's
// bindings is touched (nor asked for, which a threaded driver answers only by waiting on its
// thread) outside a run. Their storage is immutable, sized once (made again at another size); large
// writes go through a persistently mapped staging ring (the CPU copies on every core, the GPU
// copies into the buffer), small ones straight in.
//
// The kernels' programs (KernelPrograms) are one set per context, the engines made in it sharing
// them. Each is compiled by the driver once and kept as a program binary (glGetProgramBinary) under
// %LOCALAPPDATA%/GroomLab/glcache (GROOMLAB_GL_CACHE: another directory, or 0 for none), a file a
// kernel keyed by its source and the driver: loaded in milliseconds where compiling takes seconds
// when the driver's own cache misses (the first use, a new driver, a kernel an update changed: the
// others load still). Compiling runs in the background where the driver can
// (KHR_parallel_shader_compile): nothing waits for it but a run that needs a kernel not ready, which
// a host asks of first (LookEngine::kernelsReady) and draws the CPU's meanwhile.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include "groomlab/parallel.h"
#include "groomlab/look/look_engine_internal.h"

// The kernels as GLSL text, generated from shaders/look.slang by slangc (engine/gpu/CMakeLists.txt).
#include "look_kernels_glsl.h"

namespace groomlab::gpu {

namespace {

using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;
using GLuint64 = std::uint64_t;
using GLint64 = std::int64_t;
using GLsync = struct __GLsync*;

constexpr GLenum kComputeShader = 0x91B9;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kInfoLogLength = 0x8B84;
constexpr GLenum kShaderStorageBuffer = 0x90D2;
constexpr GLenum kShaderStorageBinding = 0x90D3;
constexpr GLenum kShaderStorageStart = 0x90D4;
constexpr GLenum kShaderStorageSize = 0x90D5;
constexpr GLenum kUniformBuffer = 0x8A11;
constexpr GLenum kUniformBinding = 0x8A28;
constexpr GLenum kUniformStart = 0x8A29;
constexpr GLenum kUniformSize = 0x8A2A;
constexpr GLenum kCurrentProgram = 0x8B8D;
constexpr GLenum kBufferSize = 0x8764;
constexpr GLbitfield kShaderStorageBarrier = 0x00002000;
constexpr GLbitfield kVertexAttribArrayBarrier = 0x00000001;
constexpr GLbitfield kBufferUpdateBarrier = 0x00000200;
constexpr GLenum kTimestamp = 0x8E28;
constexpr GLenum kQueryResult = 0x8866;
constexpr GLenum kQueryResultAvailable = 0x8867;
constexpr GLenum kMajorVersion = 0x821B;
constexpr GLenum kMinorVersion = 0x821C;
constexpr GLenum kSyncGpuCommandsComplete = 0x9117;
constexpr GLbitfield kSyncFlushCommands = 0x00000001;
constexpr GLenum kTimeoutExpired = 0x911B;
constexpr GLenum kWaitFailed = 0x911D;
// Buffer storage (ARB_buffer_storage, core in 4.4).
constexpr GLbitfield kMapWrite = 0x0002;
constexpr GLbitfield kMapPersistent = 0x0040;
constexpr GLbitfield kMapCoherent = 0x0080;
constexpr GLbitfield kDynamicStorage = 0x0100;
constexpr GLbitfield kClientStorage = 0x0200;
// Program binaries (ARB_get_program_binary, core in 4.1).
constexpr GLenum kProgramBinaryRetrievableHint = 0x8257;
constexpr GLenum kProgramBinaryLength = 0x8741;
constexpr GLenum kNumProgramBinaryFormats = 0x87FE;
// Compiling in the background (KHR_parallel_shader_compile, ARB_parallel_shader_compile).
constexpr GLenum kCompletionStatus = 0x91B1;
constexpr GLenum kNumExtensions = 0x821D;
// The app's point texture as an image the texture kernel writes.
constexpr GLenum kWriteOnly = 0x88B9;
constexpr GLenum kRgba32f = 0x8814;
constexpr GLuint kImageBinding = 0;  // look.slang gOutTexture under GROOMLAB_SLANG_GL (image units are their own namespace)
constexpr GLbitfield kTextureFetchBarrier = 0x00000008;
constexpr GLbitfield kShaderImageAccessBarrier = 0x00000020;

// The functions past OpenGL 1.1 the engine calls, from the current context's driver.
struct Gl {
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void(APIENTRY* DeleteShader)(GLuint);
    void(APIENTRY* DeleteProgram)(GLuint);
    void(APIENTRY* UseProgram)(GLuint);
    GLboolean(APIENTRY* IsProgram)(GLuint);
    void(APIENTRY* ProgramParameteri)(GLuint, GLenum, GLint);
    void(APIENTRY* GetProgramBinary)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
    void(APIENTRY* ProgramBinary)(GLuint, GLenum, const void*, GLsizei);
    void(APIENTRY* DispatchCompute)(GLuint, GLuint, GLuint);
    void(APIENTRY* MemoryBarrier)(GLbitfield);
    void(APIENTRY* BindBufferBase)(GLenum, GLuint, GLuint);
    void(APIENTRY* BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
    void(APIENTRY* BindBuffer)(GLenum, GLuint);
    void(APIENTRY* DeleteBuffers)(GLsizei, const GLuint*);
    void(APIENTRY* CreateBuffers)(GLsizei, GLuint*);
    void(APIENTRY* NamedBufferStorage)(GLuint, GLsizeiptr, const void*, GLbitfield);
    void(APIENTRY* NamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, const void*);
    void(APIENTRY* GetNamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, void*);
    void(APIENTRY* GetNamedBufferParameteriv)(GLuint, GLenum, GLint*);
    void(APIENTRY* CopyNamedBufferSubData)(GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr);
    void*(APIENTRY* MapNamedBufferRange)(GLuint, GLintptr, GLsizeiptr, GLbitfield);
    void(APIENTRY* GenQueries)(GLsizei, GLuint*);
    void(APIENTRY* DeleteQueries)(GLsizei, const GLuint*);
    void(APIENTRY* QueryCounter)(GLuint, GLenum);
    void(APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, GLuint64*);
    void(APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*);
    GLsync(APIENTRY* FenceSync)(GLenum, GLbitfield);
    GLenum(APIENTRY* ClientWaitSync)(GLsync, GLbitfield, GLuint64);
    void(APIENTRY* DeleteSync)(GLsync);
    void(APIENTRY* GetIntegeri_v)(GLenum, GLuint, GLint*);
    void(APIENTRY* GetInteger64i_v)(GLenum, GLuint, GLint64*);
    void(APIENTRY* BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    const GLubyte*(APIENTRY* GetStringi)(GLenum, GLuint);
    GLboolean(APIENTRY* IsBuffer)(GLuint);
    // KHR_parallel_shader_compile (or ARB_): null where the driver has neither.
    void(APIENTRY* MaxShaderCompilerThreads)(GLuint) = nullptr;

    // Loads them all from the current context; false when one is missing.
    bool load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            PROC p = wglGetProcAddress(name);
            if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) ||
                p == reinterpret_cast<PROC>(-1)) {
                ok = false;
                return;
            }
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        get(CreateShader, "glCreateShader");
        get(ShaderSource, "glShaderSource");
        get(CompileShader, "glCompileShader");
        get(GetShaderiv, "glGetShaderiv");
        get(GetShaderInfoLog, "glGetShaderInfoLog");
        get(CreateProgram, "glCreateProgram");
        get(AttachShader, "glAttachShader");
        get(LinkProgram, "glLinkProgram");
        get(GetProgramiv, "glGetProgramiv");
        get(GetProgramInfoLog, "glGetProgramInfoLog");
        get(DeleteShader, "glDeleteShader");
        get(DeleteProgram, "glDeleteProgram");
        get(UseProgram, "glUseProgram");
        get(IsProgram, "glIsProgram");
        get(ProgramParameteri, "glProgramParameteri");
        get(GetProgramBinary, "glGetProgramBinary");
        get(ProgramBinary, "glProgramBinary");
        get(DispatchCompute, "glDispatchCompute");
        get(MemoryBarrier, "glMemoryBarrier");
        get(BindBufferBase, "glBindBufferBase");
        get(BindBufferRange, "glBindBufferRange");
        get(BindBuffer, "glBindBuffer");
        get(DeleteBuffers, "glDeleteBuffers");
        get(CreateBuffers, "glCreateBuffers");
        get(NamedBufferStorage, "glNamedBufferStorage");
        get(NamedBufferSubData, "glNamedBufferSubData");
        get(GetNamedBufferSubData, "glGetNamedBufferSubData");
        get(GetNamedBufferParameteriv, "glGetNamedBufferParameteriv");
        get(CopyNamedBufferSubData, "glCopyNamedBufferSubData");
        get(MapNamedBufferRange, "glMapNamedBufferRange");
        get(GenQueries, "glGenQueries");
        get(DeleteQueries, "glDeleteQueries");
        get(QueryCounter, "glQueryCounter");
        get(GetQueryObjectui64v, "glGetQueryObjectui64v");
        get(GetQueryObjectiv, "glGetQueryObjectiv");
        get(FenceSync, "glFenceSync");
        get(ClientWaitSync, "glClientWaitSync");
        get(DeleteSync, "glDeleteSync");
        get(GetIntegeri_v, "glGetIntegeri_v");
        get(GetInteger64i_v, "glGetInteger64i_v");
        get(BindImageTexture, "glBindImageTexture");
        get(GetStringi, "glGetStringi");
        get(IsBuffer, "glIsBuffer");
        if (!ok) return false;
        GLint count = 0;
        glGetIntegerv(kNumExtensions, &count);
        for (GLint i = 0; i < count && !MaxShaderCompilerThreads; ++i) {
            const auto* name = reinterpret_cast<const char*>(GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (!name) continue;
            const bool khr = std::strcmp(name, "GL_KHR_parallel_shader_compile") == 0;
            if (!khr && std::strcmp(name, "GL_ARB_parallel_shader_compile") != 0) continue;
            PROC p = wglGetProcAddress(khr ? "glMaxShaderCompilerThreadsKHR" : "glMaxShaderCompilerThreadsARB");
            if (p && p != reinterpret_cast<PROC>(1) && p != reinterpret_cast<PROC>(2) && p != reinterpret_cast<PROC>(3) &&
                p != reinterpret_cast<PROC>(-1)) {
                MaxShaderCompilerThreads = reinterpret_cast<decltype(MaxShaderCompilerThreads)>(p);
            }
        }
        return true;
    }
};

// The context current's OpenGL version is 4.5 or later (the kernels' GLSL), else why not.
bool version45(std::string* why) {
    GLint major = 0, minor = 0;
    glGetIntegerv(kMajorVersion, &major);
    glGetIntegerv(kMinorVersion, &minor);
    if (major > 4 || (major == 4 && minor >= 5)) return true;
    *why = "GPU preview: OpenGL " + std::to_string(major) + "." + std::to_string(minor) + " (the kernels need 4.5)";
    return false;
}

// One indexed buffer binding (shader storage or uniform), as found.
struct IndexedBinding {
    GLint name = 0;
    GLint64 start = 0, size = 0;
};

// The state a run touches, kept and put back (Maya's renderer owns the context): the program,
// the shader storage bindings below kStorageBindings and uniform binding 0 (indexed and generic).
constexpr GLuint kStorageBindings = 14;
struct GlState {
    GLint program = 0, storage = 0, uniform = 0;
    IndexedBinding storages[kStorageBindings], uniforms[1];

    void save(const Gl& gl) {
        glGetIntegerv(kCurrentProgram, &program);
        glGetIntegerv(kShaderStorageBinding, &storage);
        glGetIntegerv(kUniformBinding, &uniform);
        for (GLuint i = 0; i < kStorageBindings; ++i) {
            indexed(gl, kShaderStorageBinding, kShaderStorageStart, kShaderStorageSize, i, &storages[i]);
        }
        indexed(gl, kUniformBinding, kUniformStart, kUniformSize, 0, &uniforms[0]);
    }
    void restore(const Gl& gl) const {
        gl.UseProgram(static_cast<GLuint>(program));
        for (GLuint i = 0; i < kStorageBindings; ++i) bind(gl, kShaderStorageBuffer, i, storages[i]);
        bind(gl, kUniformBuffer, 0, uniforms[0]);
        // Indexed binds set the generic binding too: it goes back last.
        gl.BindBuffer(kShaderStorageBuffer, static_cast<GLuint>(storage));
        gl.BindBuffer(kUniformBuffer, static_cast<GLuint>(uniform));
    }
    // (A binding of no buffer has no range to ask for: each query a threaded driver answers by
    // waiting on its thread.)
    static void indexed(const Gl& gl, GLenum binding, GLenum start, GLenum size, GLuint i, IndexedBinding* out) {
        gl.GetIntegeri_v(binding, i, &out->name);
        out->start = out->size = 0;
        if (!out->name) return;
        gl.GetInteger64i_v(start, i, &out->start);
        gl.GetInteger64i_v(size, i, &out->size);
    }
    static void bind(const Gl& gl, GLenum target, GLuint i, const IndexedBinding& b) {
        if (b.name && b.size > 0) {
            gl.BindBufferRange(target, i, static_cast<GLuint>(b.name), static_cast<GLintptr>(b.start), static_cast<GLsizeiptr>(b.size));
        } else {
            gl.BindBufferBase(target, i, static_cast<GLuint>(b.name));
        }
    }
};

// A buffer of the engine's (name 0: none yet).
struct GlBuffer {
    GLuint name = 0;
    std::size_t bytes = 0;
};

std::size_t rounded(std::size_t bytes) { return std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 4); }

// FNV-1a, 64 bits: the program cache's key.
std::uint64_t fnv(const void* data, std::size_t bytes, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Where the kernels' program binaries are kept: GROOMLAB_GL_CACHE (0 or empty: nowhere), else
// %LOCALAPPDATA%/GroomLab/glcache.
std::filesystem::path programCacheDirectory() {
    auto variable = [](const wchar_t* name, std::wstring* value) {
        wchar_t* text = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&text, &length, name) != 0 || !text) return false;
        *value = text;
        std::free(text);
        return true;
    };
    std::wstring set;
    if (variable(L"GROOMLAB_GL_CACHE", &set)) {
        if (set.empty() || set == L"0") return {};
        return std::filesystem::path(set);
    }
    std::wstring local;
    if (!variable(L"LOCALAPPDATA", &local) || local.empty()) return {};
    return std::filesystem::path(local) / L"GroomLab" / L"glcache";
}

// An environment variable's value ("" when unset).
std::string environment(const char* name) {
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, name) != 0 || !text) return {};
    std::string value = text;
    std::free(text);
    return value;
}

using Kernel = LookEngine::Kernel;
constexpr int kKernels = static_cast<int>(Kernel::Count);

// The kernels' GLSL, by Kernel.
struct Code {
    Kernel kernel;
    const char* source;
    const char* entry;
};
const Code& codeOf(int kernel) {
    static const std::vector<Code> list = [] {
        const Code codes[] = {
#define GROOMLAB_KERNEL(Name) {Kernel::Name, kLookGlsl##Name##CS, #Name "CS"},
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
        };
        std::vector<Code> byKernel(kKernels, Code{Kernel::Count, nullptr, nullptr});
        for (const Code& c : codes) byKernel[static_cast<int>(c.kernel)] = c;
        return byKernel;
    }();
    return list[static_cast<std::size_t>(kernel)];
}

// The kernels ready in some context, a bit each, and those that did not compile, as the last look at
// them found them (glLookKernelsReady).
KernelSet gReady, gFailed;  // (under gFailedMutex)
std::mutex gFailedMutex;
std::string gFailedWhy;  // the first kernel's that did not compile

// The kernels' programs of one context (and those sharing its objects), started when made: each
// loaded from its program binary in the cache, else compiled from its GLSL, both in the background
// where the driver can (each ask about one then waits for nothing: COMPLETION_STATUS), and its
// binary kept once compiled. The programs go with the context.
class KernelPrograms {
public:
    // The set of the context current: made (its kernels started) the first time asked, or again
    // when the one kept is not this context's (`check`: its sentinel read back, an engine being
    // made; a context made anew can have the old one's handle and object names).
    static std::shared_ptr<KernelPrograms> current(const Gl& gl, bool check) {
        const HGLRC context = wglGetCurrentContext();
        std::lock_guard<std::mutex> lock(storesMutex());
        std::shared_ptr<KernelPrograms>& kept = stores()[context];
        if (kept && check && !kept->holds()) kept.reset();
        if (!kept) kept = std::make_shared<KernelPrograms>(gl);
        return kept;
    }
    static void forget() {
        std::shared_ptr<KernelPrograms> gone;  // (deleted outside the lock)
        {
            std::lock_guard<std::mutex> lock(storesMutex());
            auto it = stores().find(wglGetCurrentContext());
            if (it == stores().end()) return;
            gone = std::move(it->second);
            stores().erase(it);
        }
        std::lock_guard<std::mutex> lock(gFailedMutex);
        gReady.reset();
        gFailed.reset();
    }

    explicit KernelPrograms(const Gl& gl) : gl_(gl), context_(wglGetCurrentContext()) {
        // The sentinel: a buffer holding this set's own signature (holds()).
        static std::atomic<std::uint64_t> made{0};
        signature_[0] = reinterpret_cast<std::uintptr_t>(this);
        signature_[1] = ++made ^ 0x9E3779B97F4A7C15ull;
        gl_.CreateBuffers(1, &sentinel_);
        gl_.NamedBufferStorage(sentinel_, sizeof signature_, signature_, 0);
        GLint formats = 0;
        glGetIntegerv(kNumProgramBinaryFormats, &formats);
        const std::filesystem::path dir = formats > 0 ? programCacheDirectory() : std::filesystem::path();
        parallel_ = gl_.MaxShaderCompilerThreads != nullptr;
        // GROOMLAB_GL_SALT: a constant of its own in every kernel, so the driver's cache misses (a
        // cold compile measured; their binaries kept apart).
        const std::string salt = environment("GROOMLAB_GL_SALT");
        for (int k = 0; k < kKernels; ++k) {
            sources_[k] = codeOf(k).source;
            if (salt.empty()) continue;
            char line[64];
            std::snprintf(line, sizeof line, "const uint kGroomLabSalt%016llx = 1u;\n",
                          static_cast<unsigned long long>(fnv(salt.data(), salt.size())));
            sources_[k].insert(sources_[k].find('\n') + 1, line);  // (after #version)
        }
        // The binaries first (each handed over before any is asked about: each ask waits on a
        // threaded driver), then the kernels compiled.
        for (int k = 0; k < kKernels; ++k) {
            if (!dir.empty()) files_[k] = dir / cacheName(k, sources_[k]);
            programs_[k] = gl_.CreateProgram();
            state_[k] = State::Pending;
            loaded_[k] = !files_[k].empty() && loadBinary(k);
        }
        for (int k = 0; k < kKernels; ++k) {
            if (!loaded_[k]) compile(k);
        }
        if (!parallel_) {  // (the driver compiles as asked: done here)
            for (int k = 0; k < kKernels; ++k) {
                while (state_[k] == State::Pending) finish(k);
            }
        }
    }
    // Its objects deleted where the context current holds them (else they go with the context).
    ~KernelPrograms() {
        if (!wglGetCurrentContext() || !holds()) return;
        for (int k = 0; k < kKernels; ++k) {
            if (shaders_[k]) gl_.DeleteShader(shaders_[k]);
            if (programs_[k]) gl_.DeleteProgram(programs_[k]);
        }
        gl_.DeleteBuffers(1, &sentinel_);
    }
    KernelPrograms(const KernelPrograms&) = delete;
    KernelPrograms& operator=(const KernelPrograms&) = delete;

    GLuint program(Kernel k) const { return programs_[static_cast<int>(k)]; }
    // Whether every kernel came from the program cache.
    bool fromCache() const {
        for (int k = 0; k < kKernels; ++k) {
            if (!cached_[k]) return false;
        }
        return true;
    }
    // Whether kernel k is ready; `wait`: once it is (else the driver asked about the pending ones
    // now and then: kPollMs apart). Throws std::runtime_error when it did not compile.
    // (One handed its binary is waited for: milliseconds.)
    bool ready(Kernel kernel, bool wait) {
        const int k = static_cast<int>(kernel);
        while (state_[k] == State::Pending && (wait || loaded_[k])) finish(k);
        if (state_[k] == State::Pending) poll();
        if (state_[k] == State::Failed) throw std::runtime_error("GPU preview: " + failed_[k]);
        return state_[k] == State::Ready;
    }
    // Whether all are ready (asking the driver about those compiling now and then); never throws.
    bool allReady() {
        if (pending_ == 0) return true;
        for (int k = 0; k < kKernels; ++k) {
            while (state_[k] == State::Pending && loaded_[k]) finish(k);
        }
        poll();
        return pending_ == 0;
    }

private:
    enum class State : std::uint8_t { Pending, Ready, Failed };
    static constexpr double kPollMs = 30.0;

    static std::map<HGLRC, std::shared_ptr<KernelPrograms>>& stores() {
        static std::map<HGLRC, std::shared_ptr<KernelPrograms>> map;
        return map;
    }
    static std::mutex& storesMutex() {
        static std::mutex m;
        return m;
    }

    // Whether the context current holds this set's objects: its sentinel reads back its signature.
    bool holds() const {
        if (!gl_.IsBuffer(sentinel_)) return false;
        std::uint64_t read[2] = {};
        gl_.GetNamedBufferSubData(sentinel_, 0, sizeof read, read);
        return std::memcmp(read, signature_, sizeof read) == 0;
    }

    // The cache file of kernel k on this driver: look_<entry>_<key>.bin, the key its source's and
    // the driver's.
    static std::wstring cacheName(int k, const std::string& source) {
        const Code& c = codeOf(k);
        std::uint64_t key = fnv("groomlab look kernel 2", 22);
        key = fnv(source.data(), source.size(), key);
        for (const GLenum what : {GLenum(GL_VENDOR), GLenum(GL_RENDERER), GLenum(GL_VERSION)}) {
            const auto* text = reinterpret_cast<const char*>(glGetString(what));
            if (text) key = fnv(text, std::strlen(text), key);
        }
        char name[96];
        std::snprintf(name, sizeof name, "look_%s_%016llx.bin", c.entry, static_cast<unsigned long long>(key));
        return std::filesystem::path(name).wstring();
    }

    // File: "GLPB", the binary's format, its length, its bytes.
    bool loadBinary(int k) {
        std::ifstream in(files_[k], std::ios::binary | std::ios::ate);
        if (!in) return false;
        const std::streamoff size = in.tellg();
        if (size <= 12) return false;
        std::vector<char> data(static_cast<std::size_t>(size));
        in.seekg(0);
        if (!in.read(data.data(), size)) return false;
        std::uint32_t head[3];
        std::memcpy(head, data.data(), sizeof head);
        if (head[0] != 0x42504C47u || head[2] != data.size() - 12) return false;
        gl_.ProgramBinary(programs_[k], head[1], data.data() + 12, static_cast<GLsizei>(head[2]));
        return true;
    }
    void saveBinary(int k) {
        if (files_[k].empty()) return;
        GLint length = 0;
        gl_.GetProgramiv(programs_[k], kProgramBinaryLength, &length);
        if (length <= 0) return;
        std::vector<char> data(12 + static_cast<std::size_t>(length));
        GLsizei got = 0;
        GLenum format = 0;
        gl_.GetProgramBinary(programs_[k], length, &got, &format, data.data() + 12);
        if (got <= 0) return;
        const std::uint32_t head[3] = {0x42504C47u, format, static_cast<std::uint32_t>(got)};
        std::memcpy(data.data(), head, sizeof head);
        // Written beside, then moved over (another process may read it meanwhile); the kernel's
        // older files go (and the single file of every kernel an older GroomLab kept).
        const std::filesystem::path& file = files_[k];
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        const std::filesystem::path partial = file.parent_path() / (file.filename().wstring() + L"." + std::to_wstring(GetCurrentProcessId()));
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(data.data(), static_cast<std::streamsize>(12 + got));
            if (!out) return;
        }
        if (!MoveFileExW(partial.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::filesystem::remove(partial, error);
            return;
        }
        const std::wstring own = L"look_" + std::filesystem::path(codeOf(k).entry).wstring() + L"_";
        for (const auto& e : std::filesystem::directory_iterator(file.parent_path(), error)) {
            const std::wstring n = e.path().filename().wstring();
            if (e.path() == file || e.path().extension() != L".bin") continue;
            const bool older = n.rfind(own, 0) == 0;
            const bool single = n.size() == 25 && n.rfind(L"look_", 0) == 0 && n.find(L'_', 5) == std::wstring::npos;
            if (older || single) std::filesystem::remove(e.path(), error);
        }
    }

    // Kernel k compiled from its GLSL (in the background where the driver can).
    void compile(int k) {
        if (parallel_ && !threadsAsked_) {
            gl_.MaxShaderCompilerThreads(0xFFFFFFFFu);  // (as many as the driver likes: the default)
            threadsAsked_ = true;
        }
        const char* source = sources_[k].c_str();
        shaders_[k] = gl_.CreateShader(kComputeShader);
        gl_.ShaderSource(shaders_[k], 1, &source, nullptr);
        gl_.CompileShader(shaders_[k]);
        gl_.ProgramParameteri(programs_[k], kProgramBinaryRetrievableHint, GL_TRUE);
        gl_.AttachShader(programs_[k], shaders_[k]);
        gl_.LinkProgram(programs_[k]);
    }

    // Asks the driver about the pending kernels (kPollMs apart at most): those done finished.
    void poll() {
        const auto now = std::chrono::steady_clock::now();
        if (gone_ || std::chrono::duration<double, std::milli>(now - polled_).count() < kPollMs) return;
        polled_ = now;
        // (A context made anew with this one's handle names other objects: nothing of it is
        // asked about, nor kept in the cache; the next engine made starts a set of its own.)
        if (!holds()) {
            gone_ = true;
            return;
        }
        for (int k = 0; k < kKernels; ++k) {
            if (state_[k] != State::Pending) continue;
            GLint done = 1;
            if (parallel_) gl_.GetProgramiv(programs_[k], kCompletionStatus, &done);
            if (done) finish(k);
        }
    }

    // Kernel k's load or compile done (waiting for it where it is not): ready, compiled from source
    // (a binary the driver no longer takes: a new driver), or failed.
    void finish(int k) {
        GLint linked = 0;
        gl_.GetProgramiv(programs_[k], kLinkStatus, &linked);
        if (loaded_[k]) {
            loaded_[k] = false;
            if (linked) {
                cached_[k] = true;
                markReady(k);
                return;
            }
            gl_.DeleteProgram(programs_[k]);
            programs_[k] = gl_.CreateProgram();
            compile(k);
            return;  // (pending again)
        }
        GLint compiled = 0;
        gl_.GetShaderiv(shaders_[k], kCompileStatus, &compiled);
        std::string why;
        if (!compiled) {
            why = std::string(codeOf(k).entry) + " did not compile: " + log(shaders_[k], true);
        } else if (!linked) {
            why = std::string(codeOf(k).entry) + " did not link: " + log(programs_[k], false);
        }
        gl_.DeleteShader(shaders_[k]);  // (flagged: it goes with its program)
        shaders_[k] = 0;
        if (!why.empty()) {
            state_[k] = State::Failed;
            failed_[k] = why;
            --pending_;
            std::lock_guard<std::mutex> lock(gFailedMutex);
            gFailed.set(static_cast<std::size_t>(k));
            if (gFailedWhy.empty()) gFailedWhy = why;
            return;
        }
        saveBinary(k);
        markReady(k);
    }
    void markReady(int k) {
        state_[k] = State::Ready;
        --pending_;
        std::lock_guard<std::mutex> lock(gFailedMutex);
        gReady.set(static_cast<std::size_t>(k));
    }
    std::string log(GLuint object, bool shader) {
        GLint length = 0;
        (shader ? gl_.GetShaderiv : gl_.GetProgramiv)(object, kInfoLogLength, &length);
        std::string text(static_cast<std::size_t>(std::max(length, 1)), '\0');
        (shader ? gl_.GetShaderInfoLog : gl_.GetProgramInfoLog)(object, length, nullptr, text.data());
        return text.c_str();
    }

    Gl gl_;
    HGLRC context_ = nullptr;
    GLuint sentinel_ = 0;
    std::uint64_t signature_[2] = {};
    bool parallel_ = false, threadsAsked_ = false;
    bool gone_ = false;  // the context current no longer holds its objects (poll)
    GLuint programs_[kKernels] = {}, shaders_[kKernels] = {};
    State state_[kKernels] = {};
    bool loaded_[kKernels] = {};  // handed its binary, not yet asked about
    bool cached_[kKernels] = {};  // from its binary
    std::string failed_[kKernels];
    std::string sources_[kKernels];
    std::filesystem::path files_[kKernels];
    int pending_ = kKernels;
    std::chrono::steady_clock::time_point polled_{};
};

class GLLookEngine final : public LookEngine {
public:
    GLLookEngine() {
        context_ = wglGetCurrentContext();
        if (!context_) throw std::runtime_error("GPU preview: no OpenGL context current");
        std::string why;
        if (!version45(&why)) throw std::runtime_error(why);
        if (!gl_.load()) throw std::runtime_error("GPU preview: OpenGL compute functions missing");
        programs_ = KernelPrograms::current(gl_, true);  // (started, not waited for)
        start();
    }
    // A sibling (LookEngine::sibling): the same context and programs, buffers and queries of its own.
    GLLookEngine(const GLLookEngine& parent, int) : gl_(parent.gl_), context_(parent.context_), programs_(parent.programs_) {
        if (wglGetCurrentContext() != context_) throw std::runtime_error("GPU preview: no OpenGL context current");
        start();
    }
    void start() {
        gl_.GenQueries(static_cast<GLsizei>(kMaxStamps), queries_);
        queryContext_ = context_;
        verified_ = context_;
        // A buffer holding this engine's own signature: whether a context shares its objects is
        // whether it reads that back (object names alone repeat from share group to share group).
        static std::uint64_t engines = 0;
        signature_[0] = reinterpret_cast<std::uintptr_t>(this);
        signature_[1] = ++engines ^ 0x9E3779B97F4A7C15ull;
        make(sentinel_, sizeof signature_);
        gl_.NamedBufferSubData(sentinel_.name, 0, sizeof signature_, signature_);
    }

    ~GLLookEngine() override {
        // Shared objects go from any context of the group; none current (Maya tearing its
        // renderer down): nothing to delete them in, they go with the context.
        if (!wglGetCurrentContext()) return;
        for (auto& [id, b] : buffers_) {
            if (b.name) gl_.DeleteBuffers(1, &b.name);
        }
        for (GlBuffer* b : {&constants_, &sentinel_}) {
            if (b->name) gl_.DeleteBuffers(1, &b->name);
        }
        for (GLsync& f : staging_.fences) {
            if (f) gl_.DeleteSync(f);
        }
        if (staging_.name) gl_.DeleteBuffers(1, &staging_.name);  // (unmapped with it)
        if (queryContext_ == wglGetCurrentContext()) gl_.DeleteQueries(static_cast<GLsizei>(kMaxStamps), queries_);
    }

    GpuApi api() const override { return GpuApi::OpenGL; }
    void* device() const override { return context_; }
    bool kernelsFromCache() const override { return programs_->fromCache(); }

    std::unique_ptr<LookEngine> sibling() override { return std::make_unique<GLLookEngine>(*this, 0); }
    void borrow(int id, LookEngine& from, int fromId) override {
        auto& other = dynamic_cast<GLLookEngine&>(from);
        const GLuint name = other.has(fromId) ? other.nameOf(fromId) : 0;
        if (!name) throw std::runtime_error("GPU preview: a sibling's buffer to borrow is missing");
        release(id);
        borrowed_[id] = name;
    }

    std::size_t videoBytes() const override {
        std::size_t n = constants_.bytes;
        for (const auto& [id, b] : buffers_) n += b.bytes;
        return n;
    }

    // Maya's viewports share one group; a new renderer, or a context lost, does not.
    bool reachable() const override { return wglGetCurrentContext() && gl_.IsProgram(programs_->program(Kernel::Interpolate)); }

    void dropTargets() override {
        for (GLuint& t : target_) t = 0;
        uvs_ = 0;
        texture_ = 0;
    }

    void finish() override {
        GLsync sync = gl_.FenceSync(kSyncGpuCommandsComplete, 0);
        if (!sync) return;
        waitFor(sync);
        gl_.DeleteSync(sync);
    }

protected:
    void write(int id, const void* data, std::size_t bytes, bool) override {
        requireReachable();
        GlBuffer& b = buffers_[id];
        make(b, bytes);
        upload(b.name, 0, data, bytes);
    }
    void writeRange(int id, std::size_t offset, const void* data, std::size_t bytes) override {
        auto it = buffers_.find(id);
        if (it == buffers_.end() || !bytes) return;
        requireReachable();
        upload(it->second.name, offset, data, bytes);
    }
    void allocate(int id, std::size_t bytes) override {
        GlBuffer& b = buffers_[id];
        if (b.name && b.bytes == rounded(bytes)) return;
        requireReachable();
        make(b, bytes);
    }
    bool has(int id) const override {
        if (id >= kOut0 && id <= kOut3) return out_[id - kOut0] != 0;
        if (id == kOutUvs) return uvs_ != 0;
        return buffers_.count(id) != 0 || borrowed_.count(id) != 0;
    }
    void release(int id) override {
        if (id >= kOut0 && id <= kOut3) out_[id - kOut0] = 0;
        borrowed_.erase(id);  // (the sibling's own)
        auto it = buffers_.find(id);
        if (it == buffers_.end()) return;
        if (it->second.name && wglGetCurrentContext()) gl_.DeleteBuffers(1, &it->second.name);
        buffers_.erase(it);
    }

    bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t uvBytes) override {
        // The app's point texture: bound as an image for the texture kernel's dispatch.
        texture_ = name(targets.pointTexture);
        // The lines' uvs, written in place where they hold every point (else left alone).
        uvs_ = 0;
        if (const GLuint uvs = name(targets.uvs)) {
            if (sizeOf(uvs) >= uvBytes) uvs_ = uvs;
        }
        // Each output: the target (any buffer can be bound as shader storage), else the engine's own.
        const GLuint want[4] = {name(targets.positions), name(targets.normals), name(targets.tangents), name(targets.bitangents)};
        for (int i = 0; i < 4; ++i) {
            target_[i] = 0;
            if (i > 0 && !frames) {
                out_[i] = 0;
                continue;
            }
            if (want[i]) {
                if (sizeOf(want[i]) < bytes) throw std::runtime_error("GPU preview: a target buffer is smaller than the points");
                out_[i] = target_[i] = want[i];
            } else {
                allocate(kOut0 + i, bytes);
                out_[i] = buffers_[kOut0 + i].name;
            }
        }
        return want[0] != 0;
    }
    void copyToTargets(bool, std::size_t) override {}  // written in place
    void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) override {
        if (!bytes) return;
        gl_.CopyNamedBufferSubData(nameOf(from), nameOf(to), static_cast<GLintptr>(fromOffset), static_cast<GLintptr>(toOffset),
                                   static_cast<GLsizeiptr>(bytes));
        gl_.MemoryBarrier(kShaderStorageBarrier | kBufferUpdateBarrier);
    }
    void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) override {
        // One buffer, a slot of kConstantSlotBytes per dispatch (bound by range: the uniform
        // offset alignment is 256 at most).
        std::vector<std::uint8_t> bytes(count * kConstantSlotBytes, 0);
        for (std::size_t i = 0; i < count; ++i) std::memcpy(bytes.data() + i * kConstantSlotBytes, static_cast<const char*>(slots) + i * slotBytes, slotBytes);
        if (constants_.bytes < bytes.size()) make(constants_, bytes.size());
        gl_.NamedBufferSubData(constants_.name, 0, static_cast<GLsizeiptr>(bytes.size()), bytes.data());
    }
    void dispatch(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY) override {
        gl_.UseProgram(programs_->program(kernel));
        gl_.BindBufferRange(kUniformBuffer, 0, constants_.name, static_cast<GLintptr>(slot * kConstantSlotBytes),
                            static_cast<GLsizeiptr>(sizeof(StageConstants)));
        auto bind = [&](GLuint binding, int id) { gl_.BindBufferBase(kShaderStorageBuffer, binding, id >= 0 && has(id) ? nameOf(id) : 0); };
        bind(1, b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets));
        bind(2, kStrandData);
        bind(3, b.in);
        bind(4, b.out[0]);
        bind(5, b.out[1]);
        bind(6, b.out[2]);
        bind(7, kRamps);
        bind(8, kFields);
        bind(9, b.aux[0]);
        bind(10, b.aux[1]);
        bind(12, b.aux[2]);
        bind(13, b.out[0] == kPointStrands ? -1 : static_cast<int>(kPointStrands));
        if (b.texture) {
            if (!texture_) throw std::runtime_error("GPU preview: no point texture bound");
            gl_.BindImageTexture(kImageBinding, texture_, 0, GL_FALSE, 0, kWriteOnly, kRgba32f);
        }
        if (groupsX && groupsY) gl_.DispatchCompute(static_cast<GLuint>(groupsX), static_cast<GLuint>(groupsY), 1);
        // The next kernel reads what this one wrote; the host's vertex shader fetches the texture.
        gl_.MemoryBarrier(kShaderStorageBarrier | (b.texture ? kShaderImageAccessBarrier | kTextureFetchBarrier : 0));
        if (b.texture) gl_.BindImageTexture(kImageBinding, 0, 0, GL_FALSE, 0, kWriteOnly, kRgba32f);
    }
    // (Bound once: only the constants' range changes between them.)
    void dispatchAlternating(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY,
                             std::size_t count) override {
        if (count == 0) return;
        if (b.texture) {
            LookEngine::dispatchAlternating(kernel, slot, b, groupsX, groupsY, count);
            return;
        }
        dispatch(kernel, slot, b, groupsX, groupsY);
        for (std::size_t i = 1; i < count; ++i) {
            gl_.BindBufferRange(kUniformBuffer, 0, constants_.name, static_cast<GLintptr>((slot + (i & 1)) * kConstantSlotBytes),
                                static_cast<GLsizeiptr>(sizeof(StageConstants)));
            if (groupsX && groupsY) gl_.DispatchCompute(static_cast<GLuint>(groupsX), static_cast<GLuint>(groupsY), 1);
            gl_.MemoryBarrier(kShaderStorageBarrier);
        }
    }
    void beginRun() override {
        requireReachable();
        kept_.save(gl_);
        timed_ = queryContext_ == wglGetCurrentContext();
    }
    void stamp(std::size_t index) override {
        if (timed_ && index < kMaxStamps) gl_.QueryCounter(queries_[index], kTimestamp);
    }
    void endRun(std::size_t stamps) override {
        // What Viewport 2.0 draws next reads them as vertex attributes.
        gl_.MemoryBarrier(kVertexAttribArrayBarrier | kShaderStorageBarrier | kBufferUpdateBarrier);
        kept_.restore(gl_);
        stampCount_ = std::min(stamps, kMaxStamps);
        ticksPending_ = timed_;
    }
    bool stampTicks(bool wait, std::vector<std::uint64_t>* ticks, double* frequency) override {
        if (!ticksPending_ || queryContext_ != wglGetCurrentContext() || stampCount_ == 0) return false;
        GLint available = 0;
        gl_.GetQueryObjectiv(queries_[stampCount_ - 1], kQueryResultAvailable, &available);
        if (!available && !wait) return false;
        ticks->assign(stampCount_, 0);
        for (std::size_t i = 0; i < stampCount_; ++i) gl_.GetQueryObjectui64v(queries_[i], kQueryResult, &(*ticks)[i]);
        ticksPending_ = false;
        *frequency = 1e9;  // nanoseconds
        return true;
    }
    std::vector<std::uint8_t> readBack(int id, std::size_t size) override {
        std::vector<std::uint8_t> bytes(size);
        const GLuint buffer = has(id) ? nameOf(id) : 0;
        if (!buffer || !size) return bytes;
        gl_.MemoryBarrier(kBufferUpdateBarrier);
        gl_.GetNamedBufferSubData(buffer, 0, static_cast<GLsizeiptr>(size), bytes.data());
        return bytes;
    }

private:
    static GLuint name(void* target) { return static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(target)); }
    GLuint nameOf(int id) const {
        if (id >= kOut0 && id <= kOut3) return out_[id - kOut0];
        if (id == kOutUvs) return uvs_;
        const auto lent = borrowed_.find(id);
        if (lent != borrowed_.end()) return lent->second;
        auto it = buffers_.find(id);
        return it == buffers_.end() ? 0 : it->second.name;
    }
    std::size_t sizeOf(GLuint buffer) const {
        GLint size = 0;
        gl_.GetNamedBufferParameteriv(buffer, kBufferSize, &size);
        return static_cast<std::size_t>(std::max(size, 0));
    }

    // Asks the driver (glIsProgram, which a threaded driver answers only by waiting on its thread)
    // once per context made current: a run's many writes then ask nothing.
    void requireReachable() {
        const HGLRC current = wglGetCurrentContext();
        if (current && current == verified_) return;
        if (!reachable()) throw std::runtime_error("GPU preview: the OpenGL context the engine was made in is gone");
        verified_ = current;
    }

    // Waits for a fence, flushing so the GPU gets to it.
    void waitFor(GLsync sync) {
        for (;;) {
            const GLenum r = gl_.ClientWaitSync(sync, kSyncFlushCommands, 1000000000ull);
            if (r != kTimeoutExpired) break;  // signaled, or the wait failed
        }
    }

    bool kernelReady(Kernel kernel, bool wait) override { return programs_->ready(kernel, wait); }

    // A buffer `bytes` long (rounded to 4), its contents undefined: immutable storage the CPU may
    // write (glNamedBufferSubData), made again at another size.
    void make(GlBuffer& b, std::size_t bytes) {
        bytes = rounded(bytes);
        if (b.name && b.bytes == bytes) return;
        if (b.name) gl_.DeleteBuffers(1, &b.name);
        b.name = 0;
        gl_.CreateBuffers(1, &b.name);
        gl_.NamedBufferStorage(b.name, static_cast<GLsizeiptr>(bytes), nullptr, kDynamicStorage);
        b.bytes = bytes;
    }

    // `bytes` of `data` into `buffer` at `offset`. Large writes through the staging ring: a slot
    // waited for (the GPU's copy out of it done), filled on every core, copied on the GPU.
    static constexpr std::size_t kStagingSlot = std::size_t{8} << 20, kStagingSlots = 4, kDirectBytes = 256 << 10;
    struct Staging {
        GLuint name = 0;
        std::uint8_t* at = nullptr;
        GLsync fences[kStagingSlots] = {};
        std::size_t next = 0;
    };
    void upload(GLuint buffer, std::size_t offset, const void* data, std::size_t bytes) {
        if (!bytes) return;
        if (bytes <= kDirectBytes || !stagingReady()) {
            gl_.NamedBufferSubData(buffer, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
            return;
        }
        const auto* from = static_cast<const std::uint8_t*>(data);
        for (std::size_t done = 0; done < bytes;) {
            const std::size_t n = std::min(kStagingSlot, bytes - done);
            const std::size_t slot = staging_.next++ % kStagingSlots;
            if (GLsync& fence = staging_.fences[slot]) {
                waitFor(fence);
                gl_.DeleteSync(fence);
                fence = nullptr;
            }
            std::uint8_t* to = staging_.at + slot * kStagingSlot;
            constexpr std::size_t kPiece = 1 << 20;
            parallelFor(0, (n + kPiece - 1) / kPiece, [&](std::size_t i) {
                const std::size_t a = i * kPiece;
                std::memcpy(to + a, from + done + a, std::min(kPiece, n - a));
            }, 1);
            gl_.CopyNamedBufferSubData(staging_.name, buffer, static_cast<GLintptr>(slot * kStagingSlot), static_cast<GLintptr>(offset + done),
                                       static_cast<GLsizeiptr>(n));
            staging_.fences[slot] = gl_.FenceSync(kSyncGpuCommandsComplete, 0);
            done += n;
        }
    }
    // The staging ring, made at the first large write: persistently mapped, coherent (what the CPU
    // wrote is seen by the next copy), in the driver's system memory.
    bool stagingReady() {
        if (staging_.at) return true;
        if (stagingFailed_) return false;
        const GLbitfield flags = kMapWrite | kMapPersistent | kMapCoherent;
        gl_.CreateBuffers(1, &staging_.name);
        gl_.NamedBufferStorage(staging_.name, static_cast<GLsizeiptr>(kStagingSlot * kStagingSlots), nullptr, flags | kClientStorage);
        staging_.at = static_cast<std::uint8_t*>(gl_.MapNamedBufferRange(staging_.name, 0, static_cast<GLsizeiptr>(kStagingSlot * kStagingSlots), flags));
        if (!staging_.at) {
            gl_.DeleteBuffers(1, &staging_.name);
            staging_.name = 0;
            stagingFailed_ = true;
        }
        return staging_.at != nullptr;
    }

    Gl gl_{};
    HGLRC context_ = nullptr, queryContext_ = nullptr, verified_ = nullptr;
    std::shared_ptr<KernelPrograms> programs_;  // the context's (its siblings' too)
    std::map<int, GlBuffer> buffers_;
    std::map<int, GLuint> borrowed_;  // buffers of a sibling's (borrow)
    GlBuffer constants_;               // a slot per dispatch of a run
    Staging staging_;
    bool stagingFailed_ = false;
    GLuint uvs_ = 0;  // the lines' uvs target this run (kOutUvs), written in place
    GLuint out_[4] = {}, target_[4] = {};  // kOut0-3 this run (a target or the engine's own); the targets
    GLuint texture_ = 0;               // the app's point texture this run (0: none)
    GlBuffer sentinel_;                // holds signature_ (reachable())
    std::uint64_t signature_[2] = {};
    GLuint queries_[kMaxStamps] = {};
    GlState kept_;
    bool timed_ = false, ticksPending_ = false;
    std::size_t stampCount_ = 0;
};

}  // namespace

std::unique_ptr<LookEngine> makeGLLookEngine() { return std::make_unique<GLLookEngine>(); }

bool prepareGLLookKernels() {
    if (!wglGetCurrentContext()) return false;
    try {
        // (The functions as the engine loads them: the context's driver's.)
        static thread_local Gl gl{};
        static thread_local HGLRC loadedFor = nullptr;
        const HGLRC context = wglGetCurrentContext();
        if (loadedFor != context) {
            std::string why;
            if (!version45(&why) || !gl.load()) return false;
            loadedFor = context;
        }
        return KernelPrograms::current(gl, false)->allReady();
    } catch (...) {
        return false;
    }
}

bool glLookKernelsReady(const LookRun& run, std::string* why) {
    const KernelSet need = kernelsOf(run);
    std::lock_guard<std::mutex> lock(gFailedMutex);
    const KernelSet failed = need & gFailed, compiling = need & ~gReady & ~failed;
    if (why && failed.any()) {
        *why = "GPU preview: " + gFailedWhy;
    } else if (why && compiling.any()) {
        *why = "the GPU's kernels are compiling (" + kernelNames(compiling) + ")";
    }
    return failed.none() && compiling.none();
}

bool glLookKernelsAllReady() {
    std::lock_guard<std::mutex> lock(gFailedMutex);
    return gReady.all();
}

void forgetGLLookKernels() {
    if (wglGetCurrentContext()) KernelPrograms::forget();
}

}  // namespace groomlab::gpu
