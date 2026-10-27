// SPDX-License-Identifier: Apache-2.0
#pragma once

// What shaders/look.slang's kernels take from the core in their C++ form (slangc's
// generated/look_kernels_cpu.cpp, which includes this first): the atomics they use on a buffer's word (its
// cpuAtomic* helpers expand to these), each on the 32-bit word at byte `at` of buffer `b` (slangc's
// RWByteAddressBuffer: its `data`), returning the word before, as the GPU's Interlocked* do; and the
// core's portable float transcendentals (groomlab_pm_*: cmake/portable_cpu_kernels.cmake puts them
// in place of the prelude's F32_sin...), so the kernels round as the operators do.

#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "groomlab/noise.h"
#include "groomlab/portable_math.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// slang-cpp-prelude.h (included after this) knows no WebAssembly target: for Emscripten the
// platform is taken as Linux's (its libc is POSIX), and <float.h> is read here first, without
// __STDC_WANT_IEC_60559_TYPES_EXT__, so the prelude's half stays its own struct (wasm32 has no
// _Float16).
#if defined(__EMSCRIPTEN__)
#include <float.h>
#ifndef SLANG_PLATFORM
#define SLANG_PLATFORM
#define SLANG_LINUX 1
#endif
#endif

namespace groomlab::gpu::cpu_atomics {

inline volatile std::uint32_t* wordAt(std::uint32_t* data, std::uint32_t at) { return data + (at >> 2); }

inline std::uint32_t compareExchange(volatile std::uint32_t* p, std::uint32_t compare, std::uint32_t v) {
#if defined(_MSC_VER)
    return static_cast<std::uint32_t>(
        _InterlockedCompareExchange(reinterpret_cast<volatile long*>(p), static_cast<long>(v), static_cast<long>(compare)));
#else
    std::uint32_t expected = compare;
    __atomic_compare_exchange_n(p, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#endif
}

inline std::uint32_t add(volatile std::uint32_t* p, std::uint32_t v) {
#if defined(_MSC_VER)
    return static_cast<std::uint32_t>(_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(p), static_cast<long>(v)));
#else
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
#endif
}

inline std::uint32_t bitOr(volatile std::uint32_t* p, std::uint32_t v) {
#if defined(_MSC_VER)
    return static_cast<std::uint32_t>(_InterlockedOr(reinterpret_cast<volatile long*>(p), static_cast<long>(v)));
#else
    return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
#endif
}

// Min and max (unsigned): a compare-exchange until the word holds the lesser (greater).
template <bool kMin>
inline std::uint32_t extreme(volatile std::uint32_t* p, std::uint32_t v) {
    std::uint32_t seen = *p;
    for (;;) {
        if (kMin ? seen <= v : seen >= v) return seen;
        const std::uint32_t was = compareExchange(p, seen, v);
        if (was == seen) return was;
        seen = was;
    }
}

}  // namespace groomlab::gpu::cpu_atomics

template <class Buffer>
inline std::uint32_t glAtomicAdd(Buffer b, std::uint32_t at, std::uint32_t v) {
    return groomlab::gpu::cpu_atomics::add(groomlab::gpu::cpu_atomics::wordAt(b.data, at), v);
}
template <class Buffer>
inline std::uint32_t glAtomicOr(Buffer b, std::uint32_t at, std::uint32_t v) {
    return groomlab::gpu::cpu_atomics::bitOr(groomlab::gpu::cpu_atomics::wordAt(b.data, at), v);
}
template <class Buffer>
inline std::uint32_t glAtomicMin(Buffer b, std::uint32_t at, std::uint32_t v) {
    return groomlab::gpu::cpu_atomics::extreme<true>(groomlab::gpu::cpu_atomics::wordAt(b.data, at), v);
}
template <class Buffer>
inline std::uint32_t glAtomicMax(Buffer b, std::uint32_t at, std::uint32_t v) {
    return groomlab::gpu::cpu_atomics::extreme<false>(groomlab::gpu::cpu_atomics::wordAt(b.data, at), v);
}
template <class Buffer>
inline std::uint32_t glAtomicCompareExchange(Buffer b, std::uint32_t at, std::uint32_t compare, std::uint32_t v) {
    return groomlab::gpu::cpu_atomics::compareExchange(groomlab::gpu::cpu_atomics::wordAt(b.data, at), compare, v);
}

#define GROOMLAB_PM_KERNEL1(name)     inline float groomlab_pm_##name(float x) { return groomlab::pm::name(x); }
#define GROOMLAB_PM_KERNEL2(name)     inline float groomlab_pm_##name(float a, float b) { return groomlab::pm::name(a, b); }
GROOMLAB_PM_KERNEL1(sin)
GROOMLAB_PM_KERNEL1(cos)
GROOMLAB_PM_KERNEL1(tan)
GROOMLAB_PM_KERNEL1(asin)
GROOMLAB_PM_KERNEL1(acos)
GROOMLAB_PM_KERNEL1(atan)
GROOMLAB_PM_KERNEL1(sinh)
GROOMLAB_PM_KERNEL1(cosh)
GROOMLAB_PM_KERNEL1(tanh)
GROOMLAB_PM_KERNEL1(exp)
GROOMLAB_PM_KERNEL1(exp2)
GROOMLAB_PM_KERNEL1(log)
GROOMLAB_PM_KERNEL1(log2)
GROOMLAB_PM_KERNEL1(log10)
GROOMLAB_PM_KERNEL1(cbrt)
GROOMLAB_PM_KERNEL2(atan2)
GROOMLAB_PM_KERNEL2(pow)
#undef GROOMLAB_PM_KERNEL1
#undef GROOMLAB_PM_KERNEL2

// The prelude's F32_floor (F64_floor), F32_min and F32_max without their library calls (floorf, fminf, fmaxf:
// calls in the kernels' loops, through the runtime's imports): floor's result exactly (signed zero,
// NaN and the integral range as they come), min and max as the core's std::min and std::max compare
// (the same as fminf and fmaxf where only the first is NaN: clamp's x).
inline float groomlab_floor(float x) {
    if (!(std::fabs(x) < 8388608.0f) || x == 0.0f) return x;  // integral already (or NaN, infinite, a signed zero)
    const float t = static_cast<float>(static_cast<std::int32_t>(x));  // toward zero
    return t > x ? t - 1.0f : t;
}
inline double groomlab_floor64(double x) {
    if (!(std::fabs(x) < 4503599627370496.0) || x == 0.0) return x;  // integral already (or NaN, infinite, a signed zero)
    const double t = static_cast<double>(static_cast<std::int64_t>(x));  // toward zero
    return t > x ? t - 1.0 : t;
}
inline float groomlab_min(float a, float b) { return b < a ? b : a; }
inline float groomlab_max(float a, float b) { return a < b ? b : a; }

// look.slang's NoiseCache on the CPU: the core's VectorNoise (noise.cpp: its lattice, hashes and
// gradients groom_math.h's, as vectorNoise's on the GPU; each octave's cell kept along a strand)
// in the kernel's own words. `C` the cache (a pointer or a reference, as slangc passes an inout).
template <class C>
inline groomlab::VectorNoise& groomlab_noise_of(C& cache) {
    using Words = std::remove_pointer_t<std::remove_reference_t<C>>;
    static_assert(sizeof(Words) >= sizeof(groomlab::VectorNoise) + alignof(groomlab::VectorNoise), "look.slang NOISE_CACHE_WORDS: too few");
    void* at = std::is_pointer_v<std::remove_reference_t<C>> ? static_cast<void*>(cache) : static_cast<void*>(&cache);
    std::size_t room = sizeof(Words);
    return *static_cast<groomlab::VectorNoise*>(std::align(alignof(groomlab::VectorNoise), sizeof(groomlab::VectorNoise), at, room));
}
template <class C, class Seeds, class Weights>
inline void groomlab_noise_start(C&& cache, std::uint32_t octaves, const Seeds& seeds, const Weights& weights) {
    static_assert(std::is_trivially_destructible_v<groomlab::VectorNoise>);
    std::uint64_t s[groomlab::VectorNoise::kMaxOctaves];
    float w[groomlab::VectorNoise::kMaxOctaves];
    const int count = static_cast<int>(octaves < groomlab::VectorNoise::kMaxOctaves ? octaves : groomlab::VectorNoise::kMaxOctaves);
    for (int o = 0; o < count; ++o) {
        s[o] = (static_cast<std::uint64_t>(seeds[o].y) << 32) | seeds[o].x;
        w[o] = weights[o];
    }
    groomlab::VectorNoise& noise = groomlab_noise_of(cache);
    new (&noise) groomlab::VectorNoise(s, count, w);
}
template <class C, class V>
inline V groomlab_noise_at(C&& cache, const V& p) {
    const groomlab::Vec3f n = groomlab_noise_of(cache)(groomlab::Vec3f(p.x, p.y, p.z));
    V out;
    out.x = n.x;
    out.y = n.y;
    out.z = n.z;
    return out;
}

// groom_math.h's mul64 on a 64-bit value held as two words (low, high): the native product's.
template <class V>
inline V groomlab_mul64(V a, V b) {
    const std::uint64_t x = (static_cast<std::uint64_t>(a.y) << 32) | a.x, y = (static_cast<std::uint64_t>(b.y) << 32) | b.x;
    const std::uint64_t r = x * y;
    V out;
    out.x = static_cast<std::uint32_t>(r);
    out.y = static_cast<std::uint32_t>(r >> 32);
    return out;
}

// look.slang FlowCS's queries on the CPU (its hostFlow*): the core's FlowField::at and atRoot and
// ScalpMesh::closestPoint on the engine's look_run.h FlowHost, its address in two words (low, high);
// look_engine_cpu.cpp's. `out`: a pointer or a reference, as slangc passes one.
namespace groomlab::gpu::cpu_host {
void flowAt(std::uint64_t host, const float p[3], const float normal[3], float out[3]);
void flowAtRoot(std::uint64_t host, std::uint32_t face, float u, float v, const float normal[3], float out[3]);
bool flowClosest(std::uint64_t host, const float p[3], float reach, std::uint32_t* face, float uv[2], float at[3], float normal[3]);
// look.slang's Collide on the CPU over a deformed surface (hostCollide*): RestPoseCollision::cursor
// and operator_state.h collideRestPush on the engine's look_run.h CollideHost, a strand's walk state
// in 9 words (look_engine_cpu.cpp restState).
void collideCursor(std::uint64_t host, std::uint32_t mesh, std::uint32_t strand, const float p[3], float state[9]);
bool collidePush(std::uint64_t host, std::uint32_t mesh, float p[3], float to, bool soft, float state[9]);
// Whether CollideHost's mesh's field (a still one's) settles a strand rooted at `root` out to
// `within`: its bounds there above `reach` (the offset plus the softness) or beyond the search,
// each by two of its cells' diagonals.
bool collideSettled(std::uint64_t host, std::uint32_t mesh, const float root[3], float within, float reach);
// operator_state.h hairGridFloorCursor and hairGridFloorPush on the engine's look_run.h
// HairGridInputs (its layout's floorSurface, its roots), the walk's state in collidePush's 9 words.
void gridFloorCursor(std::uint64_t host, std::uint32_t strand, const float p[3], float state[9]);
// distance_field.h meshPushOf on operator_state.h InterpolateClearance's mesh (its beside), the
// probe's height and distance learnt (probe[2]); the delta into `delta`.
bool clearanceMeshPush(std::uint64_t host, const float p[3], float offset, float maxDepth, float searchRadius, float probe[2], float delta[3]);
// distance_field.h CollisionSurface::push on InterpolateClearance's surface (its probe: beside, the
// field's answers alone, relaxed), the walk's cursor (vertex, anchor xyz, clear: cursor[5]).
bool clearanceSurfacePush(std::uint64_t host, const float p[3], float offset, float maxDepth, float searchRadius, float probe[2],
                          float cursor[5], float delta[3]);
bool gridFloorPush(std::uint64_t host, float p[3], float state[9]);
template <class T>
inline T& ref(T& x) { return x; }
template <class T>
inline T& ref(T* x) { return *x; }
template <class W>
inline std::uint64_t address(const W& words) { return (static_cast<std::uint64_t>(words.y) << 32) | words.x; }
}  // namespace groomlab::gpu::cpu_host

template <class W, class V>
inline V groomlab_flow_at(const W& host, const V& p, const V& normal) {
    const float at[3] = {p.x, p.y, p.z}, n[3] = {normal.x, normal.y, normal.z};
    float d[3];
    groomlab::gpu::cpu_host::flowAt(groomlab::gpu::cpu_host::address(host), at, n, d);
    V out;
    out.x = d[0];
    out.y = d[1];
    out.z = d[2];
    return out;
}
template <class W, class U, class V>
inline V groomlab_flow_at_root(const W& host, std::uint32_t face, const U& uv, const V& normal) {
    const float n[3] = {normal.x, normal.y, normal.z};
    float d[3];
    groomlab::gpu::cpu_host::flowAtRoot(groomlab::gpu::cpu_host::address(host), face, uv.x, uv.y, n, d);
    V out;
    out.x = d[0];
    out.y = d[1];
    out.z = d[2];
    return out;
}
template <class W, class V, class F, class U, class A, class N>
inline bool groomlab_flow_closest(const W& host, const V& p, float reach, F&& face, U&& uv, A&& at, N&& normal) {
    using groomlab::gpu::cpu_host::ref;
    const float q[3] = {p.x, p.y, p.z};
    std::uint32_t f = 0;
    float w[2], a[3], n[3];
    if (!groomlab::gpu::cpu_host::flowClosest(groomlab::gpu::cpu_host::address(host), q, reach, &f, w, a, n)) return false;
    ref(face) = f;
    ref(uv).x = w[0];
    ref(uv).y = w[1];
    ref(at).x = a[0];
    ref(at).y = a[1];
    ref(at).z = a[2];
    ref(normal).x = n[0];
    ref(normal).y = n[1];
    ref(normal).z = n[2];
    return true;
}
template <class A, class B, class C>
inline void groomlab_collide_state_out(const A& a, const B& b, const C& c, float w[9]) {
    using groomlab::gpu::cpu_host::ref;
    w[0] = ref(a).x;
    w[1] = ref(a).y;
    w[2] = ref(a).z;
    w[3] = ref(a).w;
    w[4] = ref(b).x;
    w[5] = ref(b).y;
    w[6] = ref(b).z;
    w[7] = ref(b).w;
    w[8] = ref(c);
}
template <class A, class B, class C>
inline void groomlab_collide_state_in(A&& a, B&& b, C&& c, const float w[9]) {
    using groomlab::gpu::cpu_host::ref;
    ref(a).x = w[0];
    ref(a).y = w[1];
    ref(a).z = w[2];
    ref(a).w = w[3];
    ref(b).x = w[4];
    ref(b).y = w[5];
    ref(b).z = w[6];
    ref(b).w = w[7];
    ref(c) = w[8];
}
template <class W, class V, class A, class B, class C>
inline void groomlab_collide_cursor(const W& host, std::uint32_t mesh, std::uint32_t strand, const V& p, A&& a, B&& b, C&& c) {
    float w[9];
    groomlab_collide_state_out(a, b, c, w);
    const float q[3] = {p.x, p.y, p.z};
    groomlab::gpu::cpu_host::collideCursor(groomlab::gpu::cpu_host::address(host), mesh, strand, q, w);
    groomlab_collide_state_in(a, b, c, w);
}
template <class W, class V, class A, class B, class C>
inline bool groomlab_collide_push(const W& host, std::uint32_t mesh, V&& p, float to, bool soft, A&& a, B&& b, C&& c) {
    using groomlab::gpu::cpu_host::ref;
    float w[9];
    groomlab_collide_state_out(a, b, c, w);
    float q[3] = {ref(p).x, ref(p).y, ref(p).z};
    const bool moved = groomlab::gpu::cpu_host::collidePush(groomlab::gpu::cpu_host::address(host), mesh, q, to, soft, w);
    ref(p).x = q[0];
    ref(p).y = q[1];
    ref(p).z = q[2];
    groomlab_collide_state_in(a, b, c, w);
    return moved;
}
template <class W, class V>
inline bool groomlab_collide_settled(const W& host, std::uint32_t mesh, const V& root, float within, float reach) {
    const float r[3] = {root.x, root.y, root.z};
    return groomlab::gpu::cpu_host::collideSettled(groomlab::gpu::cpu_host::address(host), mesh, r, within, reach);
}
template <class W, class V, class F, class D>
inline bool groomlab_clearance_mesh_push(const W& host, const V& p, float offset, float maxDepth, float searchRadius, F&& heightLow, F&& distanceLow,
                                         D&& delta) {
    using groomlab::gpu::cpu_host::ref;
    const float q[3] = {p.x, p.y, p.z};
    float probe[2] = {ref(heightLow), ref(distanceLow)}, d[3] = {0.0f, 0.0f, 0.0f};
    const bool moved = groomlab::gpu::cpu_host::clearanceMeshPush(groomlab::gpu::cpu_host::address(host), q, offset, maxDepth, searchRadius, probe, d);
    ref(heightLow) = probe[0];
    ref(distanceLow) = probe[1];
    ref(delta).x = d[0];
    ref(delta).y = d[1];
    ref(delta).z = d[2];
    return moved;
}
template <class W, class V, class F, class U, class A, class D>
inline bool groomlab_clearance_surface_push(const W& host, const V& p, float offset, float maxDepth, float searchRadius, F&& heightLow,
                                            F&& distanceLow, U&& vertex, A&& anchor, F&& clear, D&& delta) {
    using groomlab::gpu::cpu_host::ref;
    const float q[3] = {p.x, p.y, p.z};
    float probe[2] = {ref(heightLow), ref(distanceLow)}, d[3] = {0.0f, 0.0f, 0.0f};
    float cursor[5];
    const std::uint32_t v = ref(vertex);
    std::memcpy(&cursor[0], &v, 4);
    cursor[1] = ref(anchor).x;
    cursor[2] = ref(anchor).y;
    cursor[3] = ref(anchor).z;
    cursor[4] = ref(clear);
    const bool moved =
        groomlab::gpu::cpu_host::clearanceSurfacePush(groomlab::gpu::cpu_host::address(host), q, offset, maxDepth, searchRadius, probe, cursor, d);
    ref(heightLow) = probe[0];
    ref(distanceLow) = probe[1];
    std::uint32_t back;
    std::memcpy(&back, &cursor[0], 4);
    ref(vertex) = back;
    ref(anchor).x = cursor[1];
    ref(anchor).y = cursor[2];
    ref(anchor).z = cursor[3];
    ref(clear) = cursor[4];
    ref(delta).x = d[0];
    ref(delta).y = d[1];
    ref(delta).z = d[2];
    return moved;
}
template <class W, class V, class A, class B, class C>
inline void groomlab_grid_floor_cursor(const W& host, std::uint32_t strand, const V& p, A&& a, B&& b, C&& c) {
    float w[9];
    groomlab_collide_state_out(a, b, c, w);
    const float q[3] = {p.x, p.y, p.z};
    groomlab::gpu::cpu_host::gridFloorCursor(groomlab::gpu::cpu_host::address(host), strand, q, w);
    groomlab_collide_state_in(a, b, c, w);
}
template <class W, class V, class A, class B, class C>
inline bool groomlab_grid_floor_push(const W& host, V&& p, A&& a, B&& b, C&& c) {
    using groomlab::gpu::cpu_host::ref;
    float w[9];
    groomlab_collide_state_out(a, b, c, w);
    float q[3] = {ref(p).x, ref(p).y, ref(p).z};
    const bool moved = groomlab::gpu::cpu_host::gridFloorPush(groomlab::gpu::cpu_host::address(host), q, w);
    ref(p).x = q[0];
    ref(p).y = q[1];
    ref(p).z = q[2];
    groomlab_collide_state_in(a, b, c, w);
    return moved;
}
