#pragma once
// KerfKite's use of a GPU, through the OpenCL library a GPU driver installs. Nothing of OpenCL is compiled in or
// shipped: the system's library is looked for when first needed (OpenCL.dll in the Windows system folder,
// libOpenCL.so.1 on Linux, the OpenCL framework on macOS), and the few functions used are declared in opencl.cpp
// from the public specification. When there is no library, no device fit to use, or any error, the caller computes
// on the CPU; a GPU is never required.
//
// What is fit to use is decided once per process, by the environment variable KERFKITE_GPU:
//   auto (or unset)  a discrete GPU with double precision; integrated GPUs share the CPU's memory and were
//                    measured slower than the CPU's own threads, so they are left out
//   on               any device with double precision, a discrete GPU first (for tests and measurements)
//   off              never
//
// A kernel is the caller's arithmetic written in OpenCL C. To give the same bits as the CPU it must use double
// precision, begin with "#pragma OPENCL FP_CONTRACT OFF" (no fused multiply-add), and spell std::max and std::min
// out as (a < b ? b : a) and (b < a ? b : a), so that a signed zero is chosen the same way. Callers still check a
// sample of each result against the CPU, as the relief contact does.

#include <cstddef>
#include <string>
#include <vector>

namespace kerfkite {
namespace gpu {

// One argument of a kernel, in the kernel's order.
struct Argument {
    enum Kind { Int, Double, Input, Output };
    Kind kind = Int;
    int integer = 0;
    double real = 0;
    const void *input = nullptr; // Input: bytes uploaded before the run
    void *output = nullptr;      // Output: bytes read back after the run
    std::size_t bytes = 0;

    static Argument whole(int value) { Argument a; a.kind = Int; a.integer = value; return a; }
    static Argument number(double value) { Argument a; a.kind = Double; a.real = value; return a; }
    static Argument in(const void *data, std::size_t bytes) { Argument a; a.kind = Input; a.input = data; a.bytes = bytes; return a; }
    static Argument out(void *data, std::size_t bytes) { Argument a; a.kind = Output; a.output = data; a.bytes = bytes; return a; }
};

// Whether the library and a device have been looked for in this process. It does not look.
bool started();

// Whether a device is fit to use and the GPU has not been turned off. The first call looks for the library and
// the device, which takes from a few milliseconds (none there) to a few hundred (a driver starting); later calls
// are cheap.
bool available();

// The device in use, or why there is none, in one line. It does not look.
std::string describe();

// Runs kernel `name` of the OpenCL C `source` over `items` work items, one global index each. The kernel
// receives the arguments in order and then two longs: the first item of the slice it is given, and `items`. Its
// global index counts from that first item; slices are whole multiples of 64, so the kernel must ignore an index
// at or past `items`. Slices start at 64 items and grow to take about 50 ms each, so that no launch comes near a
// display driver's watchdog however slow the device. A program is built once per process. Calls from several
// threads are taken one at a time.
//
// On success `seconds` is the time from upload to read-back, without the one-off build of the program. Returns
// false, with the reason in `why`, if it could not run; the GPU is then turned off for the rest of the process,
// since a driver that failed once is not trusted again.
bool run(const char *name, const char *source, const std::vector<Argument> &arguments, std::size_t items,
         std::string &why, double &seconds);

// Turns the GPU off for the rest of the process; describe() then gives the reason.
void disable(const std::string &why);

// Seconds of GPU time per unit of work in the runs recorded so far (the caller says how much work each run was),
// or 0 before any. The caller uses it to choose between the GPU and the CPU.
double seconds_per_unit();
void record(double units, double seconds);

} // namespace gpu
} // namespace kerfkite
