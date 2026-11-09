// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace groomlab {

namespace detail {

// Calls body(context, lo, hi) over pieces [lo, hi) that cover [begin, end), side by side; never
// splits a piece of `grain` or fewer. The backend is chosen at link time, the core's code being
// the same: TBB in groomlab::core (parallel_tbb.cpp; serial, parallel_serial.cpp, in a build
// without TBB), the library's own threads in the C API's library (parallel_pool.cpp), which must
// not load a TBB a host may hold another of. A body that throws: the call throws that, once
// every piece started has ended.
using RangeBody = void (*)(const void* context, std::size_t begin, std::size_t end);
void parallelRange(std::size_t begin, std::size_t end, std::size_t grain, RangeBody body, const void* context);

// How many parallel loops' bodies this thread is inside (parallelFor's, parallelInvoke's pieces).
inline thread_local int tlsParallelDepth = 0;
struct InParallelBody {
    InParallelBody() { ++tlsParallelDepth; }
    ~InParallelBody() { --tlsParallelDepth; }
    InParallelBody(const InParallelBody&) = delete;
    InParallelBody& operator=(const InParallelBody&) = delete;
};

}  // namespace detail

// Whether this thread runs inside a parallel loop's body now. A thread waiting on a loop it
// started may run other pieces meanwhile (the backends help with nested loops), so code holding
// a lock that a loop's bodies may take again (StrandSet's copy-on-write) starts no loop of its own
// there: inside a body, it works serially.
inline bool insideParallelBody() { return detail::tlsParallelDepth > 0; }

// How many threads the parallel loops take: every core, unless capped. GROOMLAB_THREADS in the
// environment caps them (read once, when the backend starts at the first parallel call; 0 or no
// number: no cap), and setParallelThreads caps them from then on (0: every core; past the cores:
// the cores), for a host that runs the library inside a thread arena of its own (a render farm's
// slot, a plug-in beside its host's own pool). The C API library's pool keeps its threads and lets
// the ones past the cap sleep; the core on TBB runs its loops inside an arena of that many threads
// (the host's own TBB loops are not capped); the serial build has one thread, whatever is asked.
void setParallelThreads(std::size_t threads);
std::size_t parallelThreads();

// Runs fn(context) with the parallel loops it starts (on this thread) taking at most `threads`
// threads (0, or past the cap: as the cap says), so the rest of the cores stay free for other work:
// an evaluation behind a host's UI thread (async_evaluation.h), whose host draws with TBB loops of
// its own. On TBB the loops run inside an arena of that many threads; the C API library's pool and
// the serial build run fn as it is.
void runWithThreads(std::size_t threads, void (*fn)(void* context), void* context);

// Calls body(i) for every i in [begin, end), in parallel (on TBB, or the C API library's own
// threads), serially in a build without TBB. Bodies must not depend on execution order.
//
// `grain` is the fewest iterations a task takes: a range of grain or fewer is never split, so a
// loop of 256 or fewer iterations runs on one thread at the default. That suits cheap bodies (a
// strand's few points). A loop whose every iteration is heavy work (a shaft growing hundreds of
// barbs, a card baking its texels) passes a small grain, 1 for the heaviest, to spread even a
// handful of iterations over the cores.
template <class Body>
void parallelFor(std::size_t begin, std::size_t end, const Body& body, std::size_t grain = 256) {
    if (end <= begin) return;
    if (grain == 0) grain = 1;
    if (end - begin <= grain) {  // one piece: here, with no backend call
        for (std::size_t i = begin; i < end; ++i) body(i);
        return;
    }
    detail::parallelRange(
        begin, end, grain,
        [](const void* context, std::size_t lo, std::size_t hi) {
            const Body& b = *static_cast<const Body*>(context);
            const detail::InParallelBody inside;
            for (std::size_t i = lo; i < hi; ++i) b(i);
        },
        &body);
}

// Runs a() and b() side by side (a then b without a parallel backend). They must touch disjoint
// data.
template <class A, class B>
void parallelInvoke(const A& a, const B& b) {
    struct Both {
        const A* a;
        const B* b;
    };
    const Both both{&a, &b};
    detail::parallelRange(
        0, 2, 1,
        [](const void* context, std::size_t lo, std::size_t hi) {
            const Both& two = *static_cast<const Both*>(context);
            const detail::InParallelBody inside;
            for (std::size_t i = lo; i < hi; ++i) {
                if (i == 0) {
                    (*two.a)();
                } else {
                    (*two.b)();
                }
            }
        },
        &both);
}

}  // namespace groomlab
