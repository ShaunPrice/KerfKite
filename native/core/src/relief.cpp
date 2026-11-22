// Relief finishing: studio/relief.py's prepare_relief and its cutter contact, statement for statement where
// the order of the arithmetic or of the checks shows in the result.
//
// The contact places the cutter at each segment of each raster as deep as the conservative field of cell maxima
// allows. Rasters are shared out between threads, each raster computed entirely by one thread, so the number of
// threads changes no bit. Large jobs may run on a discrete GPU instead, with the same arithmetic in OpenCL C and
// a sample of every result checked bit for bit against the processor's; see try_gpu for when.
#include "kerfkite/relief.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"  // NotPorted
#include "kerfkite/gpu.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/parallel.hpp"
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
// The processor's measured speed, in seconds a check on one thread, and the seconds the large jobs of this
// process have spent on the threads.
std::mutex speed_lock;
double cpu_seconds_per_check = 2.5e-9;
double cpu_large_seconds = 0;

// Whether to try the GPU for a job of this many checks that the processor would run on `threads` threads.
// KERFKITE_GPU=on tries it for every job and KERFKITE_GPU=off for none. Otherwise only large jobs are
// considered, and the GPU is set up as a skier rents skis until the rent would have bought them: the threads run
// the large jobs until they have taken as long as setting the GPU up does, or until one job is expected to. From
// then on each job goes to whichever has been measured faster.
bool try_gpu(double checks, unsigned threads) {
    const std::string mode = gpu::mode();
    if (mode == "off") return false;
    if (mode == "on") return gpu::available();
    if (checks < kGpuChecks) return false;
    if (!gpu::started()) {
        std::lock_guard<std::mutex> guard(speed_lock);
        if (cpu_large_seconds + checks * cpu_seconds_per_check / threads < kSetupSeconds) return false;
    }
    if (!gpu::available()) return false;
    const double seconds = gpu::seconds_per_unit();
    if (seconds <= 0) return true;
    std::lock_guard<std::mutex> guard(speed_lock);
    return seconds < cpu_seconds_per_check / threads;
}

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

// The checks the loops of contact_rows make, counted exactly with the same ranges.
double count_checks(const ReliefContact &job) {
    const double dx = job.width / job.cols, dy = job.height / job.rows, radius = job.diameter / 2;
    double total = 0;
    for (int raster = 0; raster <= job.ny; raster++) {
        const double py = job.height * raster / job.ny;
        const int lo_j = static_cast<int>(std::max(0.0, std::floor((py - radius) / dy)));
        const int hi_j = static_cast<int>(std::min(double(job.rows - 1), std::floor((py + radius) / dy)));
        if (hi_j < lo_j) continue;
        double across = 0;
        for (int segment = 0; segment < job.nx; segment++) {
            const double start = job.width * segment / job.nx, end = job.width * (segment + 1) / job.nx;
            const int lo_i = static_cast<int>(std::max(0.0, std::floor((start - radius) / dx)));
            const int hi_i = static_cast<int>(std::min(double(job.cols - 1), std::floor((end + radius) / dx)));
            if (hi_i >= lo_i) across += hi_i - lo_i + 1;
        }
        total += across * (hi_j - lo_j + 1);
    }
    return total;
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

// The depths on the GPU, or false to compute them on the processor. A spread sample of the GPU's contacts (the four
// corners and kGuardSamples - 4 others) is computed again on the processor and compared bit for bit: on any
// difference the result is not used and the GPU is turned off for the rest of the process.
bool gpu_rows(const Prepared &prepared, double checks, std::vector<double> &output) {
    const ReliefContact &job = prepared.job;
    const int nx = job.nx, ny = job.ny;
    const std::size_t items = std::size_t(nx) * (ny + 1);
    std::vector<double> contacts(items);
    const std::vector<gpu::Argument> arguments = {
        gpu::Argument::in(job.maxima.data(), job.maxima.size() * sizeof(double)),
        gpu::Argument::in(prepared.xs.data(), prepared.xs.size() * sizeof(double)),
        gpu::Argument::in(prepared.ys.data(), prepared.ys.size() * sizeof(double)),
        gpu::Argument::whole(job.rows), gpu::Argument::whole(job.cols),
        gpu::Argument::number(job.width), gpu::Argument::number(job.height),
        gpu::Argument::number(job.depth), gpu::Argument::number(job.diameter),
        gpu::Argument::number(job.allowance), gpu::Argument::whole(nx),
        gpu::Argument::whole(ny), gpu::Argument::whole(job.ball ? 1 : 0),
        gpu::Argument::out(contacts.data(), contacts.size() * sizeof(double))};
    std::string why;
    double seconds = 0;
    if (!gpu::run("relief_contact", kReliefKernel, arguments, items, why, seconds)) return false;
    std::vector<std::size_t> sample = {0, std::size_t(nx - 1), std::size_t(ny) * nx, items - 1};
    for (std::size_t k = 1; sample.size() < std::min(kGuardSamples, items); k++)
        sample.push_back(std::size_t(k * 2654435761ull % items));
    for (std::size_t item : sample) {
        const double cpu = segment_contact(prepared, static_cast<int>(item / nx), static_cast<int>(item % nx));
        if (std::memcmp(&cpu, &contacts[item], sizeof cpu) != 0) {
            gpu::disable("its relief contact differed from the CPU's at raster " + std::to_string(item / nx) +
                         ", segment " + std::to_string(item % nx));
            return false;
        }
    }
    gpu::record(checks, seconds);
    for (int raster = 0; raster <= ny; raster++)
        vertex_row(&contacts[std::size_t(raster) * nx], nx, &output[std::size_t(raster) * (nx + 1)]);
    last_path = "GPU, " + gpu::describe();
    return true;
}

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

// base64.b64decode(text, validate=True) for the text the reference reads unchanged: groups of four characters of
// the standard alphabet, with one or two = only to pad the last group. False for anything else, all of which the
// reference refuses as well; the reference's messages for those differ, but its caller gives one message for all.
// A byte of text that is not ASCII is not in the alphabet, so such text is refused here too.
bool base64(const std::string &text, std::vector<unsigned char> &out) {
    // 0 to 63 for the alphabet, 64 for =, and 255 for every other byte: a group that holds either has bit 6 or 7.
    static const std::array<unsigned char, 256> table = [] {
        std::array<unsigned char, 256> values{};
        values.fill(255);
        for (int c = 0; c < 26; ++c) {
            values[size_t('A' + c)] = static_cast<unsigned char>(c);
            values[size_t('a' + c)] = static_cast<unsigned char>(c + 26);
        }
        for (int c = 0; c < 10; ++c) values[size_t('0' + c)] = static_cast<unsigned char>(c + 52);
        values[size_t('+')] = 62;
        values[size_t('/')] = 63;
        values[size_t('=')] = 64;
        return values;
    }();
    const size_t size = text.size();
    out.clear();
    if (size % 4) return false;
    if (size == 0) return true;
    const auto *in = reinterpret_cast<const unsigned char *>(text.data());
    // The = that end the text; any other = is refused below with the bytes that are not the alphabet.
    const size_t padding = in[size - 1] != '=' ? 0 : in[size - 2] != '=' ? 1 : 2;
    out.resize(size / 4 * 3 - padding);
    unsigned char *to = out.data();
    const size_t whole = padding ? size - 4 : size;
    for (size_t i = 0; i < whole; i += 4, to += 3) {
        const unsigned a = table[in[i]], b = table[in[i + 1]], c = table[in[i + 2]], d = table[in[i + 3]];
        if ((a | b | c | d) & 0xc0) return false;
        const uint32_t group = (a << 18) | (b << 12) | (c << 6) | d;
        to[0] = static_cast<unsigned char>(group >> 16);
        to[1] = static_cast<unsigned char>(group >> 8);
        to[2] = static_cast<unsigned char>(group);
    }
    if (padding) {
        // The last group: two or three characters of data. Bits beyond them may be set, as the reference allows.
        const unsigned a = table[in[size - 4]], b = table[in[size - 3]], c = padding == 1 ? table[in[size - 2]] : 0;
        if ((a | b | c) & 0xc0) return false;
        const uint32_t group = (a << 18) | (b << 12) | (c << 6);
        to[0] = static_cast<unsigned char>(group >> 16);
        if (padding == 1) to[1] = static_cast<unsigned char>(group >> 8);
    }
    return true;
}

}  // namespace

std::vector<double> relief_depths(const ReliefContact &job, std::string &ran) {
    const Prepared prepared(job);
    const double checks = count_checks(job);
    if (checks > kMaxChecks) throw ValueError("Native contact work exceeds 2000000000 checks");
    std::vector<double> output(std::size_t(job.nx + 1) * (job.ny + 1));
    const std::size_t rasters = std::size_t(job.ny) + 1;
    const unsigned threads = parallel::threads_for(checks, kChecksPerThread, rasters);
    bool done = false;
    try {
        if (try_gpu(checks, threads)) done = gpu_rows(prepared, checks, output);
    } catch (...) {
        done = false;
    }
    if (!done) {
        const auto began = std::chrono::steady_clock::now();
        parallel::for_ranges(rasters, threads, [&](std::size_t first, std::size_t last) {
            contact_rows(prepared, first, last, output.data());
        });
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        last_path = "CPU, " + std::to_string(threads) + (threads == 1 ? " thread" : " threads");
        std::lock_guard<std::mutex> guard(speed_lock);
        if (checks >= 10000000) cpu_seconds_per_check = 0.8 * cpu_seconds_per_check + 0.2 * (took * threads / checks);
        if (checks >= kGpuChecks) cpu_large_seconds += took;
    }
    ran = last_path;
    return output;
}

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
        if (!base64(encoded, raw) || raw.size() != bytes) throw ValueError(message);
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
                {"device", ran.compare(0, 3, "GPU") == 0 ? "gpu" : "cpu"},
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
