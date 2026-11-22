// KerfKite's GPU runtime: the system's OpenCL library, loaded when first needed. The few OpenCL types,
// constants and functions used are declared here from the public specification (Khronos OpenCL 1.2), so no
// OpenCL header or loader is compiled in, shipped or required. See kerfkite/gpu.hpp for the rules.

#include "kerfkite/gpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define KK_CLAPI __stdcall
#else
#include <dlfcn.h>
#define KK_CLAPI
#endif

namespace kerfkite {
namespace gpu {
namespace {

typedef std::int32_t cl_int;
typedef std::uint32_t cl_uint;
typedef std::int64_t cl_long;
typedef std::uint64_t cl_bitfield;
typedef struct opaque_platform *cl_platform_id;
typedef struct opaque_device *cl_device_id;
typedef struct opaque_context *cl_context;
typedef struct opaque_queue *cl_command_queue;
typedef struct opaque_program *cl_program;
typedef struct opaque_kernel *cl_kernel;
typedef struct opaque_mem *cl_mem;
typedef struct opaque_event *cl_event;

const cl_uint kDeviceType = 0x1000, kDeviceUnits = 0x1002, kDeviceName = 0x102B, kDeviceVersion = 0x102F;
const cl_uint kDeviceDouble = 0x1032, kDeviceUnifiedMemory = 0x1035, kBuildLog = 0x1183;
const cl_bitfield kTypeGpu = 1 << 2, kAllDevices = 0xFFFFFFFF;
const cl_bitfield kReadOnly = 1 << 2, kWriteOnly = 1 << 1, kCopyHost = 1 << 5;
const cl_uint kBlocking = 1;
const std::size_t kStep = 64;          // slices are whole multiples of this many items
const double kSliceSeconds = 0.05;     // what a slice should take

struct Functions {
    cl_int(KK_CLAPI *GetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *) = nullptr;
    cl_int(KK_CLAPI *GetDeviceIDs)(cl_platform_id, cl_bitfield, cl_uint, cl_device_id *, cl_uint *) = nullptr;
    cl_int(KK_CLAPI *GetDeviceInfo)(cl_device_id, cl_uint, std::size_t, void *, std::size_t *) = nullptr;
    cl_context(KK_CLAPI *CreateContext)(const std::intptr_t *, cl_uint, const cl_device_id *, void *, void *, cl_int *) = nullptr;
    cl_command_queue(KK_CLAPI *CreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int *) = nullptr;
    cl_program(KK_CLAPI *CreateProgramWithSource)(cl_context, cl_uint, const char **, const std::size_t *, cl_int *) = nullptr;
    cl_int(KK_CLAPI *BuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *, void *, void *) = nullptr;
    cl_int(KK_CLAPI *GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, std::size_t, void *, std::size_t *) = nullptr;
    cl_kernel(KK_CLAPI *CreateKernel)(cl_program, const char *, cl_int *) = nullptr;
    cl_mem(KK_CLAPI *CreateBuffer)(cl_context, cl_bitfield, std::size_t, void *, cl_int *) = nullptr;
    cl_int(KK_CLAPI *SetKernelArg)(cl_kernel, cl_uint, std::size_t, const void *) = nullptr;
    cl_int(KK_CLAPI *EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const std::size_t *, const std::size_t *,
                                           const std::size_t *, cl_uint, const cl_event *, cl_event *) = nullptr;
    cl_int(KK_CLAPI *EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, std::size_t, std::size_t, void *, cl_uint,
                                        const cl_event *, cl_event *) = nullptr;
    cl_int(KK_CLAPI *Finish)(cl_command_queue) = nullptr;
    cl_int(KK_CLAPI *ReleaseMemObject)(cl_mem) = nullptr;
};

struct State {
    bool looked = false;  // the library and devices have been looked at
    bool usable = false;  // a device is ready and the GPU has not been turned off
    std::string status = "not looked for yet";
    Functions cl;
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    std::map<std::string, cl_kernel> kernels;
    double units = 0, seconds = 0;
};

std::mutex &lock() {
    static std::mutex m;
    return m;
}

State &state() {
    static State s;
    return s;
}

void *open_library() {
#if defined(_WIN32)
    // GPU drivers install OpenCL.dll in the system folder. Looking only there means that a file of that name
    // beside the program or in the working folder is never loaded.
    return reinterpret_cast<void *>(LoadLibraryExA("OpenCL.dll", nullptr, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */));
#else
#if defined(__APPLE__)
    const char *names[] = {"/System/Library/Frameworks/OpenCL.framework/OpenCL"};
#else
    const char *names[] = {"libOpenCL.so.1", "libOpenCL.so"};
#endif
    for (const char *name : names)
        if (void *library = dlopen(name, RTLD_NOW | RTLD_LOCAL)) return library;
    return nullptr;
#endif
}

void *symbol(void *library, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

template <class T> bool bind(void *library, const char *name, T &function) {
    function = reinterpret_cast<T>(symbol(library, name));
    return function != nullptr;
}

std::string text_info(const Functions &cl, cl_device_id device, cl_uint what) {
    char text[256] = {0};
    if (cl.GetDeviceInfo(device, what, sizeof text - 1, text, nullptr) != 0) return "";
    return text;
}

// Looks for the library and a device, once. The caller holds the lock.
void look(State &s) {
    s.looked = true;
    const char *mode_text = std::getenv("KERFKITE_GPU");
    const std::string mode = mode_text ? mode_text : "auto";
    if (mode == "off") {
        s.status = "turned off by KERFKITE_GPU=off";
        return;
    }
    const bool permissive = mode == "on";
    void *library = open_library();
    if (!library) {
        s.status = "no OpenCL library on this computer";
        return;
    }
    Functions &cl = s.cl;
    if (!bind(library, "clGetPlatformIDs", cl.GetPlatformIDs) || !bind(library, "clGetDeviceIDs", cl.GetDeviceIDs) ||
        !bind(library, "clGetDeviceInfo", cl.GetDeviceInfo) || !bind(library, "clCreateContext", cl.CreateContext) ||
        !bind(library, "clCreateCommandQueue", cl.CreateCommandQueue) ||
        !bind(library, "clCreateProgramWithSource", cl.CreateProgramWithSource) ||
        !bind(library, "clBuildProgram", cl.BuildProgram) || !bind(library, "clGetProgramBuildInfo", cl.GetProgramBuildInfo) ||
        !bind(library, "clCreateKernel", cl.CreateKernel) || !bind(library, "clCreateBuffer", cl.CreateBuffer) ||
        !bind(library, "clSetKernelArg", cl.SetKernelArg) || !bind(library, "clEnqueueNDRangeKernel", cl.EnqueueNDRangeKernel) ||
        !bind(library, "clEnqueueReadBuffer", cl.EnqueueReadBuffer) || !bind(library, "clFinish", cl.Finish) ||
        !bind(library, "clReleaseMemObject", cl.ReleaseMemObject)) {
        s.status = "the OpenCL library lacks a function KerfKite uses";
        return;
    }
    cl_uint platform_count = 0;
    if (cl.GetPlatformIDs(0, nullptr, &platform_count) != 0 || platform_count == 0) {
        s.status = "no OpenCL platform on this computer";
        return;
    }
    std::vector<cl_platform_id> platforms(platform_count);
    if (cl.GetPlatformIDs(platform_count, platforms.data(), nullptr) != 0) {
        s.status = "the OpenCL platforms could not be listed";
        return;
    }
    // The best device: a discrete GPU, then an integrated one, then any other device; then the most compute
    // units; then the first listed. Every device gives the same bits, so the choice affects only speed.
    int best_rank = -1;
    cl_uint best_units = 0;
    std::string skipped;
    for (cl_platform_id platform : platforms) {
        cl_uint device_count = 0;
        if (cl.GetDeviceIDs(platform, kAllDevices, 0, nullptr, &device_count) != 0 || device_count == 0) continue;
        std::vector<cl_device_id> devices(device_count);
        if (cl.GetDeviceIDs(platform, kAllDevices, device_count, devices.data(), nullptr) != 0) continue;
        for (cl_device_id device : devices) {
            cl_bitfield type = 0, fp64 = 0;
            cl_uint units = 0, unified = 1;
            if (cl.GetDeviceInfo(device, kDeviceType, sizeof type, &type, nullptr) != 0) continue;
            if (cl.GetDeviceInfo(device, kDeviceDouble, sizeof fp64, &fp64, nullptr) != 0) fp64 = 0;
            if (cl.GetDeviceInfo(device, kDeviceUnits, sizeof units, &units, nullptr) != 0) units = 0;
            // Unknown counts as shared memory: an integrated device.
            if (cl.GetDeviceInfo(device, kDeviceUnifiedMemory, sizeof unified, &unified, nullptr) != 0) unified = 1;
            const bool gpu = (type & kTypeGpu) != 0, discrete = gpu && unified == 0;
            const std::string name = text_info(cl, device, kDeviceName);
            if (fp64 == 0) {
                skipped += (skipped.empty() ? "" : "; ") + name + " lacks double precision";
                continue;
            }
            if (!permissive && !discrete) {
                skipped += (skipped.empty() ? "" : "; ") + name + (gpu ? " is integrated" : " is not a GPU");
                continue;
            }
            const int rank = discrete ? 2 : gpu ? 1 : 0;
            if (rank > best_rank || (rank == best_rank && units > best_units)) {
                best_rank = rank;
                best_units = units;
                s.device = device;
            }
        }
    }
    if (!s.device) {
        s.status = "no OpenCL device fit to use" + (skipped.empty() ? std::string() : " (" + skipped + ")");
        return;
    }
    cl_int error = 0;
    s.context = cl.CreateContext(nullptr, 1, &s.device, nullptr, nullptr, &error);
    if (!s.context || error != 0) {
        s.status = "the OpenCL context could not be made (" + std::to_string(error) + ")";
        return;
    }
    s.queue = cl.CreateCommandQueue(s.context, s.device, 0, &error);
    if (!s.queue || error != 0) {
        s.status = "the OpenCL queue could not be made (" + std::to_string(error) + ")";
        return;
    }
    s.usable = true;
    s.status = text_info(cl, s.device, kDeviceName) + " (" + text_info(cl, s.device, kDeviceVersion) + ", " +
               std::to_string(best_units) + " compute units)";
}

State &ready() {
    State &s = state();
    if (!s.looked) look(s);
    return s;
}

void turn_off(State &s, const std::string &why) {
    s.usable = false;
    s.status = "turned off: " + why;
}

cl_kernel kernel_for(State &s, const char *name, const char *source, std::string &why) {
    auto found = s.kernels.find(name);
    if (found != s.kernels.end()) return found->second;
    cl_int error = 0;
    cl_program program = s.cl.CreateProgramWithSource(s.context, 1, &source, nullptr, &error);
    if (!program || error != 0) {
        why = "the OpenCL program could not be made (" + std::to_string(error) + ")";
        return nullptr;
    }
    // No options: in particular no relaxed or fast maths.
    error = s.cl.BuildProgram(program, 1, &s.device, "", nullptr, nullptr);
    if (error != 0) {
        char log[512] = {0};
        s.cl.GetProgramBuildInfo(program, s.device, kBuildLog, sizeof log - 1, log, nullptr);
        why = "the OpenCL program did not build (" + std::to_string(error) + "): " + log;
        return nullptr;
    }
    cl_kernel kernel = s.cl.CreateKernel(program, name, &error);
    if (!kernel || error != 0) {
        why = "the OpenCL kernel could not be made (" + std::to_string(error) + ")";
        return nullptr;
    }
    s.kernels[name] = kernel;
    return kernel;
}

} // namespace

bool started() {
    std::lock_guard<std::mutex> guard(lock());
    return state().looked;
}

bool available() {
    std::lock_guard<std::mutex> guard(lock());
    return ready().usable;
}

std::string describe() {
    std::lock_guard<std::mutex> guard(lock());
    return state().status;
}

void disable(const std::string &why) {
    std::lock_guard<std::mutex> guard(lock());
    State &s = state();
    s.looked = true;
    turn_off(s, why);
}

double seconds_per_unit() {
    std::lock_guard<std::mutex> guard(lock());
    const State &s = state();
    return s.units > 0 ? s.seconds / s.units : 0;
}

void record(double units, double seconds) {
    std::lock_guard<std::mutex> guard(lock());
    State &s = state();
    s.units += units;
    s.seconds += seconds;
}

bool run(const char *name, const char *source, const std::vector<Argument> &arguments, std::size_t items,
         std::string &why, double &seconds) {
    std::lock_guard<std::mutex> guard(lock());
    State &s = ready();
    seconds = 0;
    if (!s.usable) {
        why = s.status;
        return false;
    }
    if (items == 0) return true;
    cl_kernel kernel = kernel_for(s, name, source, why);
    if (!kernel) {
        turn_off(s, why);
        return false;
    }
    const auto began = std::chrono::steady_clock::now();
    const Functions &cl = s.cl;
    std::vector<std::pair<cl_mem, const Argument *>> buffers;
    auto release = [&] {
        for (auto &buffer : buffers) cl.ReleaseMemObject(buffer.first);
    };
    auto fail = [&](const std::string &reason) {
        release();
        why = reason;
        turn_off(s, reason);
        return false;
    };
    cl_uint index = 0;
    for (const Argument &argument : arguments) {
        cl_int error = 0;
        switch (argument.kind) {
        case Argument::Int: {
            const cl_int value = argument.integer;
            error = cl.SetKernelArg(kernel, index, sizeof value, &value);
            break;
        }
        case Argument::Double:
            error = cl.SetKernelArg(kernel, index, sizeof argument.real, &argument.real);
            break;
        case Argument::Input:
        case Argument::Output: {
            if (argument.bytes == 0) return fail("an OpenCL buffer of no bytes");
            const bool input = argument.kind == Argument::Input;
            cl_mem buffer = cl.CreateBuffer(s.context, input ? kReadOnly | kCopyHost : kWriteOnly, argument.bytes,
                                            input ? const_cast<void *>(argument.input) : nullptr, &error);
            if (!buffer || error != 0) return fail("an OpenCL buffer could not be made (" + std::to_string(error) + ")");
            buffers.emplace_back(buffer, &argument);
            error = cl.SetKernelArg(kernel, index, sizeof buffer, &buffer);
            break;
        }
        }
        if (error != 0) return fail("an OpenCL argument was refused (" + std::to_string(error) + ")");
        ++index;
    }
    const cl_long total = static_cast<cl_long>(items);
    if (cl_int error = cl.SetKernelArg(kernel, index + 1, sizeof total, &total))
        return fail("an OpenCL argument was refused (" + std::to_string(error) + ")");
    // Each slice is sized from the last one's measured time to take about kSliceSeconds, growing at most eight
    // times from one to the next, so that a misleading early measurement cannot make a launch long.
    std::size_t first = 0, slice = kStep;
    while (first < items) {
        const std::size_t rest = (items - first + kStep - 1) / kStep * kStep;
        const std::size_t count = std::min(slice, rest);
        const cl_long start = static_cast<cl_long>(first);
        cl_int error = cl.SetKernelArg(kernel, index, sizeof start, &start);
        const auto launched = std::chrono::steady_clock::now();
        if (error == 0) error = cl.EnqueueNDRangeKernel(s.queue, kernel, 1, nullptr, &count, nullptr, 0, nullptr, nullptr);
        if (error == 0) error = cl.Finish(s.queue);
        if (error != 0) return fail("an OpenCL run failed (" + std::to_string(error) + ")");
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - launched).count();
        first += count;
        const double next = std::min(double(count) * kSliceSeconds / std::max(took, 1e-6), 8.0 * double(count));
        slice = std::max(kStep, static_cast<std::size_t>(next) / kStep * kStep);
    }
    for (auto &buffer : buffers) {
        if (buffer.second->kind != Argument::Output) continue;
        const cl_int error = cl.EnqueueReadBuffer(s.queue, buffer.first, kBlocking, 0, buffer.second->bytes,
                                                  buffer.second->output, 0, nullptr, nullptr);
        if (error != 0) return fail("an OpenCL result could not be read (" + std::to_string(error) + ")");
    }
    release();
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    return true;
}

} // namespace gpu
} // namespace kerfkite
