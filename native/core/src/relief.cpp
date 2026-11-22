// Relief finishing: studio/relief.py's prepare_relief and its cutter contact, statement for statement where
// the order of the arithmetic or of the checks shows in the result.
//
// The contact places the cutter at each segment of each raster as deep as the conservative field of cell maxima
// allows. Rasters are shared out between threads, each raster computed entirely by one thread, so the number of
// threads changes no bit. Large jobs may also go to a GPU, alone or beside the threads, with the same arithmetic
// in OpenCL C and a sample of every result checked bit for bit against the processor's. relief_calibrate measures
// both on this computer and decides how to share; see choose and share_out.
#include "kerfkite/relief.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kerfkite/cam.hpp"  // NotPorted
#include "kerfkite/gpu.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/parallel.hpp"
#include "base64.hpp"
#include "project.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

// The most contact checks one job may make: about 4.5 s on one thread of the 24-core test machine (2.2 ns a
// check), a quarter of a second on all of its threads.
constexpr double kMaxChecks = 2000000000.0;
// A thread is started only for each two million checks, about 4 ms of work for one thread.
constexpr double kChecksPerThread = 2000000.0;
// Below this much work the GPU is not used: its fixed costs per job (buffers, transfers, launches, a few ms)
// would eat the gain. Measured on the test machine (24 threads, RTX 5090 Laptop GPU): 200 million checks take
// 24 ms on the threads and 11 ms on the GPU.
constexpr double kGpuChecks = 200000000.0;
// Setting a GPU up the first time in a process was measured at 90 to 290 ms. It is done once the large jobs of
// the process have spent this long on the threads, or when one job alone is expected to take this long.
constexpr double kSetupSeconds = 0.3;
// How many contacts of each GPU result are computed again on the processor and compared.
constexpr std::size_t kGuardSamples = 128;
// The reference's limits with the compiled contact: a map of 1024 points a side, 100,000 raster vertices.
constexpr int kMaxMap = 1024;
constexpr double kMaxVertices = 100000;

// The contact arithmetic of segment_contact below in OpenCL C, statement for statement, in double precision with
// no fused multiply-add and with std::max and std::min spelt out, so that it gives the processor's bits. Each
// work item is one segment of one raster, numbered raster by raster.
const char *kReliefKernel = R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF
double most(double a, double b) { return a < b ? b : a; }
double least(double a, double b) { return b < a ? b : a; }
__kernel void relief_contact(__global const double *maxima, __global const double *xs, __global const double *ys,
                             const int rows, const int cols, const double width,
                             const double height, const double depth, const double diameter,
                             const double allowance, const int nx, const int ny, const int ball,
                             __global double *contacts, const long first, const long items) {
    const long item = first + (long)get_global_id(0);
    if (item >= items) return;
    const int segment = (int)(item % nx), raster = (int)(item / nx);
    const double dx = width / cols, dy = height / rows, radius = diameter / 2;
    const double py = height * raster / ny;
    const int lo_j = (int)most(0.0, floor((py - radius) / dy));
    const int hi_j = (int)least((double)(rows - 1), floor((py + radius) / dy));
    const double start = width * segment / nx, end = width * (segment + 1) / nx;
    const int lo_i = (int)most(0.0, floor((start - radius) / dx));
    const int hi_i = (int)least((double)(cols - 1), floor((end + radius) / dx));
    double best = -depth;
    for (int j = lo_j; j <= hi_j; j++) {
        const double yd = most(0.0, most(ys[j] - py, py - ys[j + 1]));
        for (int i = lo_i; i <= hi_i; i++) {
            const double xd = most(0.0, most(xs[i] - end, start - xs[i + 1]));
            const double ds = xd * xd + yd * yd;
            if (ds > radius * radius) continue;
            const double sag = ball ? radius - sqrt(most(0.0, radius * radius - ds)) : 0.0;
            best = most(best, maxima[j * cols + i] - sag);
        }
    }
    contacts[item] = least(0.0, best + allowance);
}
)CL";

// What the last job of the calling thread ran on.
thread_local std::string last_path = "none yet";

// A contact job with the cell edges worked out once: i * dx for i = 0..cols and j * dy for j = 0..rows, each the
// product the reference computes for it.
struct Prepared {
    const ReliefContact &job;
    std::vector<double> xs, ys;
    explicit Prepared(const ReliefContact &given) : job(given) {
        const double dx = job.width / job.cols, dy = job.height / job.rows;
        xs.resize(std::size_t(job.cols) + 1);
        ys.resize(std::size_t(job.rows) + 1);
        for (int i = 0; i <= job.cols; i++) xs[i] = i * dx;
        for (int j = 0; j <= job.rows; j++) ys[j] = j * dy;
    }
};

// The checks the loops of contact_rows make, counted exactly with the same ranges: before[r] is the number made
// for rasters 0..r-1, so before[ny + 1] is the whole job.
std::vector<double> raster_checks(const ReliefContact &job) {
    const double dx = job.width / job.cols, dy = job.height / job.rows, radius = job.diameter / 2;
    double across = 0;  // the checks of one row of cells along a raster, the same for every raster
    for (int segment = 0; segment < job.nx; segment++) {
        const double start = job.width * segment / job.nx, end = job.width * (segment + 1) / job.nx;
        const int lo_i = static_cast<int>(std::max(0.0, std::floor((start - radius) / dx)));
        const int hi_i = static_cast<int>(std::min(double(job.cols - 1), std::floor((end + radius) / dx)));
        if (hi_i >= lo_i) across += hi_i - lo_i + 1;
    }
    std::vector<double> before(std::size_t(job.ny) + 2, 0.0);
    for (int raster = 0; raster <= job.ny; raster++) {
        const double py = job.height * raster / job.ny;
        const int lo_j = static_cast<int>(std::max(0.0, std::floor((py - radius) / dy)));
        const int hi_j = static_cast<int>(std::min(double(job.rows - 1), std::floor((py + radius) / dy)));
        before[raster + 1] = before[raster] + (hi_j < lo_j ? 0 : across * (hi_j - lo_j + 1));
    }
    return before;
}

// The deepest the cutter may go at one segment of one raster.
double segment_contact(const Prepared &prepared, int raster, int segment) {
    const ReliefContact &job = prepared.job;
    const int rows = job.rows, cols = job.cols, nx = job.nx, ny = job.ny;
    const double width = job.width, height = job.height;
    const double dx = width / cols, dy = height / rows, radius = job.diameter / 2;
    const double *maxima = job.maxima.data(), *xs = prepared.xs.data(), *ys = prepared.ys.data();
    const double py = height * raster / ny;
    const int lo_j = static_cast<int>(std::max(0.0, std::floor((py - radius) / dy)));
    const int hi_j = static_cast<int>(std::min(double(rows - 1), std::floor((py + radius) / dy)));
    const double start = width * segment / nx, end = width * (segment + 1) / nx;
    const int lo_i = static_cast<int>(std::max(0.0, std::floor((start - radius) / dx)));
    const int hi_i = static_cast<int>(std::min(double(cols - 1), std::floor((end + radius) / dx)));
    double best = -job.depth;
    for (int j = lo_j; j <= hi_j; j++) {
        const double yd = std::max(0.0, std::max(ys[j] - py, py - ys[j + 1]));
        for (int i = lo_i; i <= hi_i; i++) {
            const double xd = std::max(0.0, std::max(xs[i] - end, start - xs[i + 1]));
            const double ds = xd * xd + yd * yd;
            if (ds > radius * radius) continue;
            const double sag = job.ball ? radius - std::sqrt(std::max(0.0, radius * radius - ds)) : 0;
            best = std::max(best, maxima[j * cols + i] - sag);
        }
    }
    return std::min(0.0, best + job.allowance);
}

// One raster's nx + 1 depths from its nx segment contacts: each vertex takes the deeper limit of its neighbours.
void vertex_row(const double *contacts, int nx, double *row) {
    for (int i = 0; i <= nx; i++) row[i] = i == 0 ? contacts[0] : i == nx ? contacts[nx - 1] : std::max(contacts[i - 1], contacts[i]);
}

// Rasters first..last-1 of the answer: each raster is nx + 1 depths.
void contact_rows(const Prepared &prepared, std::size_t first, std::size_t last, double *output) {
    const int nx = prepared.job.nx;
    std::vector<double> contacts(nx);
    for (int raster = static_cast<int>(first); raster < static_cast<int>(last); raster++) {
        for (int segment = 0; segment < nx; segment++) contacts[segment] = segment_contact(prepared, raster, segment);
        vertex_row(contacts.data(), nx, output + std::size_t(raster) * (nx + 1));
    }
}

double since(std::chrono::steady_clock::time_point began) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
}

// What computes contacts beside the processor's threads: the GPU, through kerfkite/gpu.hpp. The self-test puts
// a stand-in in its place (relief_test_device) to exercise the sharing on computers without a GPU.
struct Device {
    bool (*started)();
    bool (*usable)();
    bool (*shared)();  // shares the processor's memory: an integrated GPU
    std::string (*name)();
    // Contacts 0..items-1, numbered raster by raster, into `out`; `seconds` from upload to read-back.
    bool (*contacts)(const Prepared &, std::size_t items, double *out, double &seconds, std::string &why);
    void (*turn_off)(const std::string &why);
};

bool gpu_contacts(const Prepared &prepared, std::size_t items, double *out, double &seconds, std::string &why) {
    const ReliefContact &job = prepared.job;
    const std::vector<gpu::Argument> arguments = {
        gpu::Argument::in(job.maxima.data(), job.maxima.size() * sizeof(double)),
        gpu::Argument::in(prepared.xs.data(), prepared.xs.size() * sizeof(double)),
        gpu::Argument::in(prepared.ys.data(), prepared.ys.size() * sizeof(double)),
        gpu::Argument::whole(job.rows), gpu::Argument::whole(job.cols),
        gpu::Argument::number(job.width), gpu::Argument::number(job.height),
        gpu::Argument::number(job.depth), gpu::Argument::number(job.diameter),
        gpu::Argument::number(job.allowance), gpu::Argument::whole(job.nx),
        gpu::Argument::whole(job.ny), gpu::Argument::whole(job.ball ? 1 : 0),
        gpu::Argument::out(out, items * sizeof(double))};
    return gpu::run("relief_contact", kReliefKernel, arguments, items, why, seconds);
}

const Device kGpu = {gpu::started, gpu::available, gpu::shared_memory, gpu::describe, gpu_contacts, gpu::disable};

// The stand-in: the processor's arithmetic on the driving thread, taking `slowdown` times as long, and wrong at
// item `wrong` when that is not negative.
std::atomic<bool> stand_in_on{true};
std::atomic<double> stand_in_slowdown{1};
std::atomic<long long> stand_in_wrong{-1};
std::atomic<double> stand_in_share{-1};  // a share every job is given, when not negative
std::string stand_in_status = "test device";
std::mutex stand_in_lock;

bool stand_in_started() { return true; }
bool stand_in_usable() { return stand_in_on; }
bool stand_in_shared() { return false; }
std::string stand_in_name() {
    std::lock_guard<std::mutex> guard(stand_in_lock);
    return stand_in_status;
}
bool stand_in_contacts(const Prepared &prepared, std::size_t items, double *out, double &seconds, std::string &why) {
    if (!stand_in_on) {
        why = stand_in_name();
        return false;
    }
    const auto began = std::chrono::steady_clock::now();
    const int nx = prepared.job.nx;
    for (std::size_t item = 0; item < items; ++item)
        out[item] = segment_contact(prepared, static_cast<int>(item / nx), static_cast<int>(item % nx));
    const long long wrong = stand_in_wrong;
    if (wrong >= 0 && static_cast<std::size_t>(wrong) < items) out[wrong] = std::nextafter(out[wrong], 1.0);
    seconds = since(began);
    const double slowdown = stand_in_slowdown;
    if (slowdown > 1) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds * (slowdown - 1)));
        seconds *= slowdown;
    }
    return true;
}
void stand_in_turn_off(const std::string &why) {
    stand_in_on = false;
    std::lock_guard<std::mutex> guard(stand_in_lock);
    stand_in_status = "turned off: " + why;
}

const Device kStandIn = {stand_in_started, stand_in_usable, stand_in_shared, stand_in_name, stand_in_contacts,
                         stand_in_turn_off};
std::atomic<const Device *> in_use{&kGpu};

// What is known of this computer's speed. Until a calibration, the GPU is set up as a skier rents skis until the
// rent would have bought them (see choose); a calibration measures the threads and the device on one job and
// says whether large jobs go to the threads, the device, or both at once.
enum class Plan { Threads, Device, Both };
struct Speeds {
    double cpu = 2.5e-9;        // seconds a check on one thread
    double large_seconds = 0;   // seconds the large jobs of this process have spent on the threads
    double device_checks = 0, device_seconds = 0;  // the device's runs so far, all costs included
    bool calibrated = false;
    double gpu = 0, fixed = 0;  // calibrated: seconds a check, and a job's fixed seconds, on the device
    Plan plan = Plan::Threads;
    unsigned beside = 1;        // calibrated: the threads to run beside the device
    json report;                // the last calibration
};
std::mutex speed_lock;
Speeds speeds;

// How a job is shared: the fraction of its checks given to the device (0: threads only, 1: device only), and the
// threads for the rest.
struct Choice {
    double share = 0;
    unsigned threads = 1;
};

// The share at which the device and `threads` threads finish together: fixed + f C g = (1 - f) C c / threads.
double even_share(double checks, double cpu, unsigned threads, double gpu, double fixed) {
    const double per_thread = cpu / threads, whole = checks * (gpu + per_thread);
    return whole > 0 ? (checks * per_thread - fixed) / whole : 0;
}

// How to run a job of `checks` that would use `threads` threads on its own. KERFKITE_GPU=on gives every job to
// the device and KERFKITE_GPU=off none.
Choice choose(double checks, unsigned threads) {
    const Device &device = *in_use.load();
    if (&device == &kStandIn && stand_in_share >= 0) return {stand_in_share, threads};
    const std::string mode = gpu::mode();
    if (mode == "off") return {0, threads};
    if (mode == "on") return {device.usable() ? 1.0 : 0.0, threads};
    Speeds known;
    {
        std::lock_guard<std::mutex> guard(speed_lock);
        known = speeds;
    }
    if (!known.calibrated) {
        // Only large jobs, and only a discrete GPU: the threads run the large jobs until they have taken as long
        // as setting the GPU up does, or until one job is expected to; from then on each job goes to whichever
        // has been measured faster.
        if (checks < kGpuChecks) return {0, threads};
        if (!device.started() && known.large_seconds + checks * known.cpu / threads < kSetupSeconds) return {0, threads};
        if (!device.usable() || device.shared()) return {0, threads};
        if (known.device_checks <= 0) return {1, threads};
        return {known.device_seconds / known.device_checks < known.cpu / threads ? 1.0 : 0.0, threads};
    }
    if (known.plan == Plan::Threads || !device.usable()) return {0, threads};
    // Each way is chosen only when it is expected to save a twentieth of the time.
    Choice best{0, threads};
    double fastest = checks * known.cpu / threads;
    const double alone = known.fixed + checks * known.gpu;
    if (alone < 0.95 * fastest) {
        best = {1, threads};
        fastest = alone;
    }
    if (known.plan == Plan::Both) {
        const unsigned beside = std::max(1u, std::min(known.beside, threads));
        const double share = even_share(checks, known.cpu, beside, known.gpu, known.fixed);
        const double both = known.fixed + share * checks * known.gpu;
        if (share > 0.02 && share < 0.98 && both < 0.95 * fastest) best = {share, beside};
    }
    return best;
}

// What a job ran on, for the speeds and the report.
struct Outcome {
    std::string path;
    unsigned threads = 0;
    double thread_checks = 0, thread_seconds = 0;
    bool device_ok = false;
    double device_checks = 0, device_seconds = 0;
};

// Whether the device's contacts agree with the processor's at a spread sample (the corners of its part and
// kGuardSamples others). On any difference the device is turned off for the rest of the process.
bool guarded(const Prepared &prepared, const std::vector<double> &contacts, const Device &device) {
    const std::size_t items = contacts.size(), nx = std::size_t(prepared.job.nx);
    std::vector<std::size_t> sample = {0, items - 1};
    if (items >= nx) {
        sample.push_back(nx - 1);
        sample.push_back(items - nx);
    }
    for (std::size_t k = 1; sample.size() < std::min(kGuardSamples, items); k++)
        sample.push_back(std::size_t(k * 2654435761ull % items));
    for (std::size_t item : sample) {
        const double cpu = segment_contact(prepared, static_cast<int>(item / nx), static_cast<int>(item % nx));
        if (std::memcmp(&cpu, &contacts[item], sizeof cpu) != 0) {
            device.turn_off("its relief contact differed from the CPU's at raster " + std::to_string(item / nx) +
                            ", segment " + std::to_string(item % nx));
            return false;
        }
    }
    return true;
}

std::string threads_text(unsigned threads) {
    return std::to_string(threads) + (threads == 1 ? " thread" : " threads");
}

// Runs a job as chosen: the device takes the first rasters, up to the chosen share of the checks, while the
// threads take the rest, each raster computed whole by one side. A device that fails or disagrees leaves its
// rasters to the threads afterwards, so the answer is always the processor's bits.
Outcome share_out(const Prepared &prepared, const std::vector<double> &before, Choice choice, std::vector<double> &output) {
    const std::size_t rasters = before.size() - 1, nx = std::size_t(prepared.job.nx);
    const double total = before.back();
    std::size_t split = 0;  // rasters [0, split) go to the device
    if (choice.share >= 1) split = rasters;
    else if (choice.share > 0) split = std::lower_bound(before.begin(), before.end(), choice.share * total) - before.begin();
    split = std::min(split, rasters);
    const Device &device = *in_use.load();
    Outcome outcome;
    std::vector<double> contacts;
    std::string why;
    auto device_part = [&] {
        try {
            contacts.assign(split * nx, 0.0);
            outcome.device_ok = device.contacts(prepared, contacts.size(), contacts.data(), outcome.device_seconds, why);
        } catch (...) {
            outcome.device_ok = false;
        }
    };
    std::thread driver;
    bool alongside = false;
    if (split > 0 && split < rasters) {
        try {
            driver = std::thread(device_part);
            alongside = true;
        } catch (...) {
            // No thread to be had: the device's part runs first, on this thread.
        }
    }
    if (split > 0 && !alongside) device_part();
    if (split < rasters) {
        try {
            const auto began = std::chrono::steady_clock::now();
            parallel::for_ranges(rasters - split, choice.threads, [&](std::size_t first, std::size_t last) {
                contact_rows(prepared, split + first, split + last, output.data());
            });
            outcome.thread_seconds = since(began);
        } catch (...) {
            if (alongside) driver.join();
            throw;
        }
        outcome.threads = choice.threads;
        outcome.thread_checks = total - before[split];
    }
    if (alongside) driver.join();
    if (split > 0 && outcome.device_ok) outcome.device_ok = guarded(prepared, contacts, device);
    if (split > 0 && outcome.device_ok) {
        for (std::size_t raster = 0; raster < split; raster++)
            vertex_row(&contacts[raster * nx], int(nx), &output[raster * (nx + 1)]);
        outcome.device_checks = before[split];
    } else if (split > 0) {
        const unsigned again = parallel::threads_for(before[split], kChecksPerThread, split);
        parallel::for_ranges(split, again, [&](std::size_t first, std::size_t last) {
            contact_rows(prepared, first, last, output.data());
        });
        outcome.threads = std::max(outcome.threads, again);
        outcome.device_seconds = 0;
    }
    if (outcome.device_ok && split == rasters) {
        outcome.path = "GPU, " + device.name();
    } else if (outcome.device_ok) {
        const int percent = static_cast<int>(std::lround(100 * before[split] / total));
        outcome.path = "GPU+CPU, " + std::to_string(percent) + "% of the work on " + device.name() + ", the rest on " +
                       threads_text(outcome.threads);
    } else {
        outcome.path = "CPU, " + threads_text(outcome.threads);
    }
    return outcome;
}

// Folds what a job measured into the speeds.
void learn(const Outcome &outcome) {
    std::lock_guard<std::mutex> guard(speed_lock);
    if (outcome.thread_checks >= 10000000 && outcome.thread_seconds > 0)
        speeds.cpu = 0.8 * speeds.cpu + 0.2 * (outcome.thread_seconds * outcome.threads / outcome.thread_checks);
    if (outcome.thread_checks >= kGpuChecks) speeds.large_seconds += outcome.thread_seconds;
    if (!outcome.device_ok) return;
    speeds.device_checks += outcome.device_checks;
    speeds.device_seconds += outcome.device_seconds;
    if (in_use.load() == &kGpu) gpu::record(outcome.device_checks, outcome.device_seconds);
    if (speeds.calibrated && outcome.device_checks >= 10000000) {
        const double per_check = (outcome.device_seconds - speeds.fixed) / outcome.device_checks;
        if (per_check > 0) speeds.gpu = 0.8 * speeds.gpu + 0.2 * per_check;
    }
}

// The calibration's job: a varied field of 512 x 512 cells over 128 mm, a 4 mm ball, and as many rasters as give
// about `checks` checks.
ReliefContact calibration_job(double checks) {
    ReliefContact job;
    job.rows = job.cols = 512;
    job.width = job.height = 128;
    job.depth = 5;
    job.diameter = 4;
    job.allowance = 0.1;
    job.ball = true;
    job.nx = 512;
    job.maxima.resize(std::size_t(job.rows) * job.cols);
    std::uint64_t state = 2026;
    for (int j = 0; j < job.rows; j++)
        for (int i = 0; i < job.cols; i++) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const double noise = double(state >> 11) / 9007199254740992.0;
            const double wave = 0.5 + 0.25 * std::sin(i * 0.031) * std::cos(j * 0.023);
            job.maxima[std::size_t(j) * job.cols + i] = -job.depth * std::min(1.0, wave + 0.25 * noise);
        }
    job.ny = 2;
    const std::vector<double> sample = raster_checks(job);
    const double per_raster = std::max(1.0, sample.back() / 3);
    job.ny = static_cast<int>(std::max(4.0, std::min(20000.0, checks / per_raster)));
    return job;
}

// One timed run of the calibration job, and whether its depths equal the threads' own.
struct Trial {
    double seconds = 0;
    Outcome outcome;
    bool same = true;
};
Trial trial(const Prepared &prepared, const std::vector<double> &before, Choice choice,
            const std::vector<double> *reference, std::vector<double> *keep = nullptr) {
    std::vector<double> output(std::size_t(prepared.job.nx + 1) * (prepared.job.ny + 1));
    Trial result;
    const auto began = std::chrono::steady_clock::now();
    result.outcome = share_out(prepared, before, choice, output);
    result.seconds = since(began);
    if (reference)
        result.same = output.size() == reference->size() &&
                      std::memcmp(output.data(), reference->data(), output.size() * sizeof(double)) == 0;
    if (keep) *keep = std::move(output);
    return result;
}

std::mutex calibration_lock;

// studio.relief._number(data, key, default, minimum=, positive=, maximum=).
double setting_of(const json &data, const char *key, const json *fallback, double minimum, bool positive,
                  double maximum) {
    const json *value = get(data, key);
    if (!value) value = fallback;
    double result = 0;
    if (!value || value->is_boolean() || !plain_number(*value, result))
        throw ValueError(std::string(key) + " must be a finite number");
    if (!std::isfinite(result) || result < minimum || result > maximum || (positive && result < 0.000001))
        throw ValueError(std::string(key) + " is outside its supported range");
    return result;
}

}  // namespace

std::vector<double> relief_depths(const ReliefContact &job, std::string &ran) {
    const Prepared prepared(job);
    const std::vector<double> before = raster_checks(job);
    const double checks = before.back();
    if (checks > kMaxChecks) throw ValueError("Native contact work exceeds 2000000000 checks");
    std::vector<double> output(std::size_t(job.nx + 1) * (job.ny + 1));
    const std::size_t rasters = std::size_t(job.ny) + 1;
    const unsigned threads = parallel::threads_for(checks, kChecksPerThread, rasters);
    Choice choice{0, threads};
    try {
        choice = choose(checks, threads);
    } catch (...) {
        choice = {0, threads};
    }
    const Outcome outcome = share_out(prepared, before, choice, output);
    learn(outcome);
    ran = last_path = outcome.path;
    return output;
}

Json relief_calibrate() {
    std::lock_guard<std::mutex> one_at_a_time(calibration_lock);
    const auto began = std::chrono::steady_clock::now();
    const unsigned threads = parallel::limit();
    const Device &device = *in_use.load();
    // A small first run sizes the job to take about 40 ms on all the threads.
    double cpu = 0;
    {
        const ReliefContact probe = calibration_job(8e6);
        const Prepared prepared(probe);
        const std::vector<double> before = raster_checks(probe);
        cpu = trial(prepared, before, {0, threads}, nullptr).seconds * threads / before.back();
    }
    const ReliefContact job = calibration_job(std::max(2e7, std::min(6e8, 0.04 * threads / std::max(cpu, 1e-12))));
    const Prepared prepared(job);
    const std::vector<double> before = raster_checks(job);
    const double checks = before.back();
    std::vector<double> reference;
    double on_threads = trial(prepared, before, {0, threads}, nullptr, &reference).seconds;
    if (on_threads < 0.1) on_threads = std::min(on_threads, trial(prepared, before, {0, threads}, &reference).seconds);
    cpu = on_threads * threads / checks;
    json report = json{{"threads", threads}, {"checks", checks}, {"threads_ms", on_threads * 1000}};

    Plan plan = Plan::Threads;
    double gpu_per_check = 0, fixed = 0, share = 0, best = on_threads;
    unsigned beside = threads;
    // A run that used the device counts only if it gave the threads' bits throughout; a difference the guard's
    // sample missed still turns the device off.
    auto counts = [&](const Trial &run) {
        if (run.outcome.device_ok && !run.same) device.turn_off("its relief contact differed from the CPU's in the calibration");
        return run.outcome.device_ok && run.same;
    };
    if (gpu::mode() != "off" && device.usable()) {
        report["device"] = device.name();
        // The device alone, twice: the first run builds its program.
        Trial run = trial(prepared, before, {1, threads}, &reference);
        bool fine = counts(run);
        double device_wall = run.seconds, device_seconds = run.outcome.device_seconds;
        if (fine) {
            run = trial(prepared, before, {1, threads}, &reference);
            fine = counts(run);
            device_wall = std::min(device_wall, run.seconds);
            device_seconds = std::min(device_seconds, run.outcome.device_seconds);
        }
        // A job of a few rasters gives the fixed cost of a run: buffers, transfers and launches.
        if (fine) {
            const ReliefContact small = calibration_job(0);
            const Prepared small_prepared(small);
            const std::vector<double> small_before = raster_checks(small);
            std::vector<double> small_reference;
            trial(small_prepared, small_before, {0, threads}, nullptr, &small_reference);
            double small_seconds = 1e9;
            for (int k = 0; k < 2 && fine; k++) {
                run = trial(small_prepared, small_before, {1, threads}, &small_reference);
                fine = counts(run);
                small_seconds = std::min(small_seconds, run.outcome.device_seconds);
            }
            if (fine) {
                gpu_per_check = std::max(1e-15, (device_seconds - small_seconds) / (checks - small_before.back()));
                fixed = std::max(0.0, small_seconds - small_before.back() * gpu_per_check);
            }
        }
        if (fine) {
            report["device_ms"] = device_wall * 1000;
            report["device_fixed_ms"] = fixed * 1000;
            if (device_wall < 0.95 * best) {
                plan = Plan::Device;
                best = device_wall;
                share = 1;
            }
            // Both at once: the device beside all the threads, and beside one fewer, since on a computer with few
            // cores the thread that drives the device may want a core to itself.
            json tried = json::array();
            for (unsigned k : {threads, threads - 1}) {
                if (k == 0 || !fine || (k == threads - 1 && threads < 2)) continue;
                double f = even_share(checks, cpu, k, gpu_per_check, fixed), fastest = 0, kept = f;
                for (int round = 0; round < 2 && f > 0.02 && f < 0.98; round++) {
                    run = trial(prepared, before, {f, k}, &reference);
                    if (!counts(run)) {
                        fine = false;
                        break;
                    }
                    if (fastest == 0 || run.seconds < fastest) {
                        fastest = run.seconds;
                        kept = f;
                    }
                    // The next share from what each side took in this run.
                    const double given = run.outcome.device_checks, rest = checks - given;
                    if (given <= 0 || rest <= 0 || run.outcome.thread_seconds <= 0) break;
                    const double device_rate = std::max(1e-15, (run.outcome.device_seconds - fixed) / given);
                    f = even_share(checks, run.outcome.thread_seconds * k / rest, k, device_rate, fixed);
                }
                if (fastest > 0) {
                    tried.push_back(json{{"threads", k}, {"share", kept}, {"ms", fastest * 1000}});
                    if (fastest < 0.95 * best) {
                        plan = Plan::Both;
                        best = fastest;
                        share = kept;
                        beside = k;
                    }
                }
            }
            report["shared"] = std::move(tried);
        }
        if (!fine || !device.usable()) {
            plan = Plan::Threads;
            share = 0;
            report["device_status"] = device.name();
        }
    } else {
        report["device"] = nullptr;
        report["device_status"] = gpu::mode() == "off" ? std::string("turned off by KERFKITE_GPU=off") : device.name();
    }
    report["plan"] = plan == Plan::Both ? "gpu+cpu" : plan == Plan::Device ? "gpu" : "threads";
    report["share"] = share;
    report["beside"] = plan == Plan::Both ? beside : plan == Plan::Device ? 0u : threads;
    report["seconds"] = since(began);
    std::lock_guard<std::mutex> guard(speed_lock);
    speeds.calibrated = true;
    speeds.cpu = cpu;
    speeds.gpu = gpu_per_check;
    speeds.fixed = fixed;
    speeds.plan = plan;
    speeds.beside = beside;
    speeds.report = report;
    return report;
}

Json relief_calibration() {
    std::lock_guard<std::mutex> guard(speed_lock);
    return speeds.calibrated ? speeds.report : Json(nullptr);
}

namespace detail {

void relief_test_device(bool on, double slowdown, long long wrong, double share) {
    {
        std::lock_guard<std::mutex> guard(stand_in_lock);
        stand_in_status = "test device";
    }
    stand_in_on = true;
    stand_in_slowdown = slowdown;
    stand_in_wrong = wrong;
    stand_in_share = share;
    in_use = on ? &kStandIn : &kGpu;
    std::lock_guard<std::mutex> guard(speed_lock);
    speeds = Speeds{};
}

}  // namespace detail

Json prepare_relief(const Json &data) {
    if (!data.is_object()) throw ValueError("Relief input must be an object");
    const json zero(0);
    const double width = setting_of(data, "width", nullptr, 0, true, 10000);
    const double height = setting_of(data, "height", nullptr, 0, true, 10000);
    const double depth = setting_of(data, "depth", nullptr, 0, true, 1000);
    const double x = setting_of(data, "x", &zero, 0, false, 10000);
    const double y = setting_of(data, "y", &zero, 0, false, 10000);
    const double diameter = setting_of(data, "tool_diameter", nullptr, 0, true, 1000);
    const double step = setting_of(data, "stepover", nullptr, 0, true, 10000);
    const double allowance = setting_of(data, "allowance", &zero, 0, false, 1000);
    if (step > diameter) throw ValueError("stepover must not exceed tool_diameter");
    if (allowance > depth) throw ValueError("allowance must not exceed depth");
    const json *shape_value = get(data, "tool_shape");
    const bool ball = !shape_value || is_text(shape_value, "ball");
    if (!ball && !is_text(shape_value, "flat")) throw ValueError("tool_shape must be flat or ball");
    const std::string shape = ball ? "ball" : "flat";
    const std::string limit = std::to_string(kMaxMap);

    // The heights as values (h - 1) * depth, row 0 at the bottom of the map.
    std::vector<double> values;
    int rows = 0, columns = 0;
    bool image = false;
    if (data.contains("heightmap_rgb")) {
        if (!none(get(data, "heightmap"))) throw ValueError("Give heightmap or heightmap_rgb, not both");
        // _rgb_heights(data, depth, limit)
        const json *size = get(data, "heightmap_size");
        // Two whole numbers (not floats, not booleans), each 2..1024.
        bool sized = size && size->is_array() && size->size() == 2;
        for (size_t k = 0; sized && k < 2; ++k) {
            const json &v = (*size)[k];
            sized = v.is_number_unsigned() ? v.get<uint64_t>() >= 2 && v.get<uint64_t>() <= uint64_t(kMaxMap)
                                           : v.is_number_integer() && v.get<int64_t>() >= 2 && v.get<int64_t>() <= kMaxMap;
        }
        if (!sized) throw ValueError("heightmap_size must be [columns, rows], each 2.." + limit);
        columns = static_cast<int>((*size)[0].get<int64_t>());
        rows = static_cast<int>((*size)[1].get<int64_t>());
        const std::string message = "heightmap_rgb must be base64 of columns x rows x 3 bytes";
        const json *text = get(data, "heightmap_rgb");
        if (!text || !text->is_string()) throw ValueError(message);
        const std::string &encoded = text->get_ref<const std::string &>();
        const size_t bytes = size_t(columns) * rows * 3;
        // len(text) counts characters and size() bytes, which differ only for text that is not ASCII; the reader
        // refuses such text whatever its length, with the same message.
        if (encoded.size() > 4 * ((bytes + 2) / 3)) throw ValueError(message);
        std::vector<unsigned char> raw;
        if (!detail::base64(encoded, raw) || raw.size() != bytes) throw ValueError(message);
        values.resize(size_t(rows) * columns);
        for (int j = 0; j < rows; ++j) {
            // The image's top row comes first; row 0 of the map is the bottom.
            const unsigned char *pixel = raw.data() + size_t(rows - 1 - j) * columns * 3;
            for (int i = 0; i < columns; ++i, pixel += 3) {
                // (0.2126 R + 0.7152 G + 0.0722 B) / 255, in numpy's order, then (luminance - 1) * depth.
                const double luminance =
                    (0.2126 * double(pixel[0]) + 0.7152 * double(pixel[1]) + 0.0722 * double(pixel[2])) / 255;
                values[size_t(j) * columns + i] = (luminance - 1) * depth;
            }
        }
        image = true;
    } else {
        const json *matrix = get(data, "heightmap");
        if (!matrix || !matrix->is_array() || matrix->size() < 2 || matrix->size() > size_t(kMaxMap))
            throw ValueError("heightmap must contain 2.." + limit + " rows");
        const json &first = (*matrix)[0];
        columns = first.is_array() ? static_cast<int>(std::min<size_t>(first.size(), size_t(kMaxMap) + 1)) : 0;
        if (columns < 2 || columns > kMaxMap) throw ValueError("heightmap must contain 2.." + limit + " columns");
        rows = static_cast<int>(matrix->size());
        values.resize(size_t(rows) * columns);
        for (int j = 0; j < rows; ++j) {
            const json &row = (*matrix)[j];
            if (!row.is_array() || row.size() != size_t(columns)) throw ValueError("heightmap must be rectangular");
            for (int i = 0; i < columns; ++i) {
                const json &cell = row[i];
                double v = 0;
                if (cell.is_boolean() || !plain_number(cell, v) || !std::isfinite(v) || !(0 <= v && v <= 1))
                    throw ValueError("heightmap values must be finite numbers in 0..1");
                values[size_t(j) * columns + i] = (v - 1) * depth;
            }
        }
    }
    const double dx = width / (columns - 1), dy = height / (rows - 1);
    // Sampling no coarser than the input X grid or the requested stepover.
    const double nx = std::ceil(width / py_min(dx, step));
    const double ny = std::ceil(height / step);
    if ((nx + 1) * (ny + 1) > kMaxVertices)
        throw ValueError("Relief exceeds 100000 vertices; increase stepover or reduce map size");
    // Bound the work before allocating paths. The exact count is bounded as well.
    const double cells_x = py_min(double(columns - 1), std::ceil((width / nx + diameter) / dx) + 2);
    const double cells_y = py_min(double(rows - 1), std::ceil(diameter / dy) + 2);
    if (nx * (ny + 1) * cells_x * cells_y > kMaxChecks)
        throw ValueError("Relief contact calculation too large; reduce map resolution or tool diameter");

    ReliefContact contact;
    contact.rows = rows - 1;
    contact.cols = columns - 1;
    contact.nx = static_cast<int>(nx);
    contact.ny = static_cast<int>(ny);
    contact.width = width;
    contact.height = height;
    contact.depth = depth;
    contact.diameter = diameter;
    contact.allowance = allowance;
    contact.ball = ball;
    contact.maxima.resize(size_t(contact.rows) * contact.cols);
    for (int j = 0; j < contact.rows; ++j)
        for (int i = 0; i < contact.cols; ++i) {
            const double a = values[size_t(j) * columns + i], b = values[size_t(j) * columns + i + 1];
            const double c = values[size_t(j + 1) * columns + i], d = values[size_t(j + 1) * columns + i + 1];
            // np.maximum of the pairs for an image, max() of the four for a list: the same value, no NaN being
            // possible and no zero of either sign but +0.
            contact.maxima[size_t(j) * contact.cols + i] = image ? std::max(std::max(a, b), std::max(c, d))
                                                                 : py_max(py_max(py_max(a, b), c), d);
        }
    std::string ran;
    const std::vector<double> depths = relief_depths(contact, ran);

    json::Array paths, depth_rows;
    paths.reserve(size_t(ny) + 1);
    depth_rows.reserve(size_t(ny) + 1);
    const int segments = contact.nx, rasters = contact.ny;
    std::vector<double> xs(size_t(segments) + 1);
    for (int i = 0; i <= segments; ++i) xs[i] = width * i / segments;
    for (int raster = 0; raster <= rasters; ++raster) {
        const double py = height * raster / rasters;
        const double *row = depths.data() + size_t(raster) * (segments + 1);
        json::Array path(size_t(segments) + 1), z(size_t(segments) + 1);
        // Odd rasters run back, the reference reversing the raster's points and depths together.
        for (int k = 0; k <= segments; ++k) {
            const int i = raster % 2 ? segments - k : k;
            path[k] = json(json::Array{json(x + xs[i]), json(y + py)});
            z[k] = json(row[i]);
        }
        paths.push_back(json(std::move(path)));
        depth_rows.push_back(json(std::move(z)));
    }
    json surface = json{
        {"kind", "bilinear-heightmap"},
        {"width", width},
        {"height", height},
        {"depth", depth},
        {"x", x},
        {"y", y},
        {"rows", rows},
        {"columns", columns},
        {"sample_spacing", json::array({dx, dy})},
        {"actual_stepover", height / ny},
        {"tool_shape", shape},
        {"tool_diameter", diameter},
        {"allowance", allowance},
        {"compensation", "conservative-cell-envelope"},
        {"backend", "cpp"},
    };
    json warnings = json::array(
        {"Top-access finishing geometry only; caller must apply safe retracts, step-downs and machine limits.",
         "Compensation bounds the supplied bilinear height field using cell maxima. Coarse samples leave extra stock; "
         "features absent from the heightmap cannot be recovered.",
         "Raster centers reach the map edges: clear the cutter-radius overhang outside the map. Outside stock, clamps, "
         "holder and undercuts are not modeled.",
         "Allowance is capped at the stock top (Z=0). Ball-cutter raster spacing can leave scallops between passes."});
    return json{{"paths", json(std::move(paths))},
                {"path_depths", json(std::move(depth_rows))},
                {"device", ran.compare(0, 8, "GPU+CPU,") == 0 ? "gpu+cpu" : ran.compare(0, 4, "GPU,") == 0 ? "gpu" : "cpu"},
                {"surface", std::move(surface)},
                {"warnings", std::move(warnings)}};
}

std::string relief_gpu_status() {
    const bool off = gpu::mode() == "off";
    if (!off) gpu::available();
    if (off && !gpu::started()) return "turned off by KERFKITE_GPU=off";
    return gpu::describe();
}

std::string last_relief_contact() { return last_path; }

bool relief_command(const std::string &command) { return command == "relief"; }

Json relief_call(const std::string &, const Json &request) { return prepare_relief(request); }

}  // namespace kerfkite
