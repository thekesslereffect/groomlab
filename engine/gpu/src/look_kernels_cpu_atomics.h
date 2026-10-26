// SPDX-License-Identifier: Apache-2.0
#pragma once

// The atomics shaders/look.slang's kernels use on a buffer's word, for their C++ form (slangc's
// look_kernels_cpu.cpp, which includes this first: its cpuAtomic* helpers expand to these). Each on
// the 32-bit word at byte `at` of buffer `b` (slangc's RWByteAddressBuffer: its `data`), returning
// the word before, as the GPU's Interlocked* do.

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
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
