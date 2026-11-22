#pragma once
// Work split over threads in a fixed way: the indices 0..count-1 are cut into contiguous ranges, and the caller
// writes each output from exactly one range with the same arithmetic it would use on one thread. A result then
// cannot depend on how many threads ran, or on which finished first.
//
// The number of threads is the hardware's count, capped by the environment variable KERFKITE_THREADS when that
// is a whole number of at least one (KERFKITE_THREADS=1 runs everything on the calling thread), and by how much
// work there is: a thread is started only for each `per_thread` units of work.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <thread>
#include <vector>

namespace kerfkite {
namespace parallel {

// The most threads any piece of work may use.
inline unsigned limit() {
    unsigned most = std::thread::hardware_concurrency();
    if (most == 0) most = 1;
    if (const char *text = std::getenv("KERFKITE_THREADS")) {
        // Digits only: no sign, no spaces, no locale.
        unsigned long value = 0;
        bool digits = *text != '\0';
        for (const char *c = text; *c && digits; ++c) {
            if (*c < '0' || *c > '9' || value > 100000) digits = false;
            else value = value * 10 + static_cast<unsigned long>(*c - '0');
        }
        if (digits && value >= 1 && value < most) most = static_cast<unsigned>(value);
    }
    return most;
}

// How many threads to use for `work` units split into `parts` indivisible pieces, giving each thread at least
// `per_thread` units.
inline unsigned threads_for(double work, double per_thread, std::size_t parts) {
    unsigned count = limit();
    const double wanted = per_thread > 0 ? work / per_thread : 1;
    if (wanted < count) count = wanted < 1 ? 1 : static_cast<unsigned>(wanted);
    if (parts < count) count = parts == 0 ? 1 : static_cast<unsigned>(parts);
    return count;
}

// Runs body(begin, end) over [0, count) cut into `threads` contiguous ranges, the first on the calling thread.
// It returns when every range is done. An exception in a range is rethrown after all have finished; if a thread
// cannot be started, its range runs on the calling thread instead.
template <class Body> void for_ranges(std::size_t count, unsigned threads, Body body) {
    if (threads <= 1 || count <= 1) {
        body(std::size_t(0), count);
        return;
    }
    if (threads > count) threads = static_cast<unsigned>(count);
    std::vector<std::exception_ptr> errors(threads);
    auto range = [&](unsigned k) {
        const std::size_t begin = count * k / threads, end = count * (k + 1) / threads;
        try {
            body(begin, end);
        } catch (...) {
            errors[k] = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    unsigned started = 1;
    try {
        for (unsigned k = 1; k < threads; ++k) {
            pool.emplace_back(range, k);
            ++started;
        }
    } catch (...) {
        // No more threads to be had: the ranges not started run below, on this thread.
    }
    range(0);
    for (unsigned k = started; k < threads; ++k) range(k);
    for (std::thread &thread : pool) thread.join();
    for (const std::exception_ptr &error : errors)
        if (error) std::rethrow_exception(error);
}

} // namespace parallel
} // namespace kerfkite
