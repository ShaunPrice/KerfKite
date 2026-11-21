// Where the time of an operation goes: a clock for each phase of the kernel.
//
// Compiled in only with KERFKITE_SOLID_PROFILE (cmake -DKERFKITE_SOLID_PROFILE=ON); without it the
// macros below are empty and the kernel carries no clock at all. A phase entered inside another stops
// the outer one's clock, so the totals add up to the time spent and no time is counted twice. The one
// exception is a phase that keeps everything inside it to itself (verification, which runs the same
// engine on the rounded result). The clocks measure; they decide nothing, so a profiled build gives the
// same solids as any other.
// native/tools/solid_bench.cpp prints the totals.
#ifndef KERFKITE_SOLID_TIMING_HPP
#define KERFKITE_SOLID_TIMING_HPP

#include <cstddef>
#include <vector>

#ifdef KERFKITE_SOLID_PROFILE
#include <chrono>
#endif

namespace kerfkite {
namespace solid {
namespace detail {

enum class Phase {
    Construct,   // primitives, extrusions, revolutions, hulls, maps
    Load,        // points, planes and boxes of the operands
    Tree,        // building the bounding-box tree
    Candidates,  // finding the pairs of triangles whose boxes overlap
    Pairs,       // intersecting those pairs exactly
    Passing,     // triangles nothing can touch: classified by patch
    Subdivide,   // triangulating the triangles that were cut
    Faces,       // merging identical triangles
    Cells,       // ordering faces around edges
    Winding,     // ray counts and winding numbers
    Extract,     // the boundary of the solid region
    Simplify,    // removing unneeded vertices
    Round,       // rounding, merging, cap flips
    Verify,      // the exact checks of the rounded result
    Canonical,   // ordering and checking the mesh of a solid
    Count
};

inline const char *timing_name(std::size_t phase) {
    static const char *const names[] = {"construction and maps", "load operands",       "bounding-box tree",   "candidate pairs",
                                        "triangle intersections", "passing triangles",  "subdivision",         "faces",
                                        "cells (order at edges)", "winding numbers",    "extraction",          "simplification",
                                        "rounding and merging",  "verification",        "canonical order"};
    return phase < static_cast<std::size_t>(Phase::Count) ? names[phase] : "";
}

#ifdef KERFKITE_SOLID_PROFILE

class PhaseTimer {
public:
    explicit PhaseTimer(Phase phase, bool whole = false) : whole_(whole), idle_(state().held > 0) {
        State &s = state();
        if (whole_) ++s.held;
        if (idle_) return;
        const double start = now();
        if (s.depth > 0) s.total[s.phase[s.depth - 1]] += start - s.since;
        if (s.depth < kDepth) s.phase[s.depth] = static_cast<int>(phase);
        ++s.depth;
        s.since = start;
    }
    ~PhaseTimer() {
        State &s = state();
        if (whole_) --s.held;
        if (idle_) return;
        const double end = now();
        --s.depth;
        if (s.depth < kDepth) s.total[s.phase[s.depth]] += end - s.since;
        s.since = end;
    }
    PhaseTimer(const PhaseTimer &) = delete;
    PhaseTimer &operator=(const PhaseTimer &) = delete;
    static double *total() { return state().total; }

private:
    enum { kDepth = 32 };
    // The phases entered and not yet left, innermost last, and when the innermost was entered or resumed.
    struct State {
        double total[static_cast<int>(Phase::Count)] = {};
        int phase[kDepth] = {};
        int depth = 0, held = 0;
        double since = 0;
    };
    bool whole_, idle_;
    static State &state() {
        static thread_local State s;
        return s;
    }
    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
};

inline void timing_reset() {
    for (int k = 0; k < static_cast<int>(Phase::Count); ++k) PhaseTimer::total()[k] = 0;
}
inline std::vector<double> timing_totals() { return std::vector<double>(PhaseTimer::total(), PhaseTimer::total() + static_cast<int>(Phase::Count)); }

#define KERFKITE_SOLID_PHASE_JOIN2(a, b) a##b
#define KERFKITE_SOLID_PHASE_JOIN(a, b) KERFKITE_SOLID_PHASE_JOIN2(a, b)
#define KERFKITE_SOLID_PHASE(name) \
    ::kerfkite::solid::detail::PhaseTimer KERFKITE_SOLID_PHASE_JOIN(kerfkite_phase_, __LINE__)(::kerfkite::solid::detail::Phase::name)
#define KERFKITE_SOLID_WHOLE_PHASE(name) \
    ::kerfkite::solid::detail::PhaseTimer KERFKITE_SOLID_PHASE_JOIN(kerfkite_phase_, __LINE__)(::kerfkite::solid::detail::Phase::name, true)

#else

inline void timing_reset() {}
inline std::vector<double> timing_totals() { return std::vector<double>(); }
#define KERFKITE_SOLID_PHASE(name) static_cast<void>(0)
#define KERFKITE_SOLID_WHOLE_PHASE(name) static_cast<void>(0)

#endif

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
