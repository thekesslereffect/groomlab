// SPDX-License-Identifier: Apache-2.0
#pragma once

// Where an evaluation's time goes, by phase: `ProfileScope scope("interpolate.bind");` around a
// phase of a node adds its wall time and one call to that label's row of a process-wide table.
// Off by default, and then a scope is one relaxed atomic read; on with the environment variable
// GROOMLAB_PROFILE=1 (read once, before main) or setProfileEnabled(true). `groomlab-cli eval
// --timings` prints the table under each frame's node times when it is on, and the app after it
// opens a groom and after each of its benchmarks (--benchmark, --benchmark-drag, --benchmark-orbit,
// --live-benchmark).
//
// The rows: a node's phases ("scatter.relax.neighbors", "braid.weaveStrands"); every node
// evaluation by type and kind, its own time ("node.Collide.full", "node.Clump.incremental":
// graph.cpp); a stage's run on the CPU look engine ("look.curl.setBase", "look.curl.run":
// look_cpu.cpp), each kernel it dispatched ("look.kernel.Clearance") and the engine's own steps
// ("look.prepare.*", "look.run.*"); a session's evaluation ("session.graph", "session.restPose")
// and a simulation's step ("simulate.substep.guides").
//
// Rules for placing a scope: around a PHASE (a gather, a bind, a kernel run), never inside a
// per-strand or per-point loop (a scope that is on reads the clock twice and takes a lock when it
// ends); a label is a string literal, dotted from the node down ("braid.weave"). Scopes nest: an
// inner label's time is also inside its outer one's, so the rows of one node do not sum to it
// unless they are siblings. A phase run from several threads at once counts each thread's time.
// A scope never changes what a node computes.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace groomlab {

struct ProfileEntry {
    std::string label;
    double milliseconds = 0.0;  // wall time of every call, summed
    std::uint64_t calls = 0;
};

namespace detail {
extern std::atomic<bool> profileOn;  // profile.cpp: GROOMLAB_PROFILE at start, then setProfileEnabled
}

inline bool profileEnabled() { return detail::profileOn.load(std::memory_order_relaxed); }
void setProfileEnabled(bool on);

// One call of `label` that took `milliseconds` (what a ProfileScope does when it ends); nothing
// while profiling is off.
void profileAdd(const char* label, double milliseconds);
// The table so far, in the order the labels were first recorded (the order of the work).
std::vector<ProfileEntry> profileReport();
// The table as text, one "label: ms (calls)" line each, indented by `indent`; empty with no rows.
std::string profileReportText(const std::string& indent = "  ");
void profileReset();

class ProfileScope {
public:
    explicit ProfileScope(const char* label) : label_(profileEnabled() ? label : nullptr) {
        if (label_) start_ = std::chrono::steady_clock::now();
    }
    ~ProfileScope() { stop(); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

    // Ends the scope before its block does (a phase that is not a block of its own); once.
    void stop() {
        if (!label_) return;
        profileAdd(label_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
        label_ = nullptr;
    }

private:
    const char* label_;
    std::chrono::steady_clock::time_point start_{};
};

// A function's phases one after another, without a block each: next() ends the phase before it and
// starts `label`'s; the last ends with stop() or with the object (a return on the way counts what
// ran until it).
class ProfilePhases {
public:
    ProfilePhases() = default;
    explicit ProfilePhases(const char* label) { next(label); }
    ~ProfilePhases() { stop(); }
    ProfilePhases(const ProfilePhases&) = delete;
    ProfilePhases& operator=(const ProfilePhases&) = delete;

    void next(const char* label) {
        stop();
        if (!profileEnabled()) return;
        label_ = label;
        start_ = std::chrono::steady_clock::now();
    }
    void stop() {
        if (!label_) return;
        profileAdd(label_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
        label_ = nullptr;
    }

private:
    const char* label_ = nullptr;
    std::chrono::steady_clock::time_point start_{};
};

}  // namespace groomlab
