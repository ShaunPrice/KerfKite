// Checks of the work split over threads (kerfkite/parallel.hpp) and of the GPU runtime (kerfkite/gpu.hpp).
// On a computer without a usable GPU the runtime must say why and refuse to run; with one, a kernel's results
// must equal the CPU's bit for bit. With --gpu-off the test expects KERFKITE_GPU=off to have turned the GPU off.

#include "kerfkite/gpu.hpp"
#include "kerfkite/parallel.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void set_threads(const char *value) {
#if defined(_WIN32)
    _putenv_s("KERFKITE_THREADS", value ? value : "");
#else
    if (value) setenv("KERFKITE_THREADS", value, 1);
    else unsetenv("KERFKITE_THREADS");
#endif
}

void parallel_checks() {
    using namespace kerfkite::parallel;
    set_threads(nullptr);  // whatever the test was started with
    const unsigned most = limit();
    check(most >= 1, "at least one thread");
    // Every index is visited exactly once, by the range the fixed split gives it.
    for (std::size_t count : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(7), std::size_t(100)}) {
        for (unsigned threads : {1u, 2u, 3u, 8u, 200u}) {
            std::vector<int> visits(count, 0);
            std::vector<int> owner(count, -1);
            for_ranges(count, threads, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    ++visits[i];
                    owner[i] = static_cast<int>(begin);
                }
            });
            bool once = true, fixed = true;
            const unsigned used = threads > count ? static_cast<unsigned>(count) : threads;
            for (std::size_t i = 0; i < count; ++i) {
                once = once && visits[i] == 1;
                if (used > 1) {
                    // Range k is [count k / n, count (k + 1) / n).
                    std::size_t k = 0;
                    while (count * (k + 1) / used <= i) ++k;
                    fixed = fixed && owner[i] == static_cast<int>(count * k / used);
                }
            }
            check(once, "each index once, count " + std::to_string(count) + " threads " + std::to_string(threads));
            check(fixed, "fixed ranges, count " + std::to_string(count) + " threads " + std::to_string(threads));
        }
    }
    // An exception in any range reaches the caller once every thread has finished.
    std::string caught;
    try {
        for_ranges(100, 4, [](std::size_t begin, std::size_t end) {
            if (begin <= 60 && 60 < end) throw std::runtime_error("range with 60");
        });
    } catch (const std::runtime_error &error) {
        caught = error.what();
    }
    check(caught == "range with 60", "exception passed on");
    // Threads only for enough work, never more than parts or the limit.
    check(threads_for(10, 100, 50) == 1, "small work, one thread");
    check(threads_for(1e12, 1, 3) <= 3, "no more threads than parts");
    check(threads_for(1e12, 1, 1000000) == most, "large work, the limit");
    set_threads("1");
    check(limit() == 1, "KERFKITE_THREADS=1");
    set_threads("0");
    check(limit() == most, "KERFKITE_THREADS=0 ignored");
    set_threads("many");
    check(limit() == most, "KERFKITE_THREADS=many ignored");
    set_threads(nullptr);
    check(limit() == most, "unset");
}

const char *kSquareRoots = R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF
__kernel void roots(__global const double *values, const double scale, const int shift, __global double *out,
                    const long first, const long items) {
    const long item = first + (long)get_global_id(0);
    if (item >= items) return;
    const double v = values[item];
    out[item] = sqrt(v * scale + shift) / 3.0 - floor(v / 7.0);
}
)CL";

void gpu_checks(bool expect_off) {
    using namespace kerfkite::gpu;
    check(!started(), "the GPU is not looked for until asked");
    const bool usable = available();
    check(started(), "looked for once asked");
    const std::string status = describe();
    std::printf("GPU: %s\n", status.c_str());
    check(!status.empty(), "a status");
    if (expect_off) {
        check(!usable && status == "turned off by KERFKITE_GPU=off", "KERFKITE_GPU=off");
    }
    const std::size_t items = 1000003;  // not a multiple of 64
    std::vector<double> values(items), out(items, -1), expected(items);
    for (std::size_t i = 0; i < items; ++i) values[i] = double(i) * 0.37 + 1.0 / double(i + 1);
    const double scale = 1.25;
    const int shift = 3;
    for (std::size_t i = 0; i < items; ++i) {
        const double v = values[i];
        expected[i] = std::sqrt(v * scale + shift) / 3.0 - std::floor(v / 7.0);
    }
    const std::vector<Argument> arguments = {Argument::in(values.data(), items * sizeof(double)),
                                             Argument::number(scale), Argument::whole(shift),
                                             Argument::out(out.data(), items * sizeof(double))};
    std::string why;
    double seconds = -1;
    const bool ran = run("roots", kSquareRoots, arguments, items, why, seconds);
    if (!usable) {
        check(!ran && why == status, "no run without a usable GPU");
        return;
    }
    check(ran, "a run on " + status + (ran ? std::string() : ": " + why));
    if (!ran) return;
    check(seconds > 0, "the run is timed");
    check(std::memcmp(out.data(), expected.data(), items * sizeof(double)) == 0, "the GPU's bits are the CPU's");
    record(double(items), seconds);
    check(seconds_per_unit() > 0, "a measured speed");
    // A program that does not build turns the GPU off, with the reason.
    check(!run("broken", "__kernel void broken( {", arguments, items, why, seconds), "a broken program fails");
    check(why.find("did not build") != std::string::npos, "the reason names the build: " + why);
    check(!available() && describe().rfind("turned off:", 0) == 0, "and the GPU is turned off: " + describe());
}

} // namespace

int main(int argc, char **argv) {
    const bool expect_off = argc > 1 && std::strcmp(argv[1], "--gpu-off") == 0;
    if (!expect_off) parallel_checks();
    gpu_checks(expect_off);
    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
