// Relief finishing from a height field without Python: studio/relief.py's prepare_relief, to the bit.
//
// A height field (heights in 0..1, or the RGB bytes of an image) is sampled on rasters across the work, and at
// each raster point the cutter (ball or flat) is placed as deep as it may go without entering the field, each
// cell of which counts as its highest corner. The result is paths and depths for studio.cam, and a record of
// the surface. The contact runs on the processor's threads, on a GPU, or on both at once for large jobs, with the
// same arithmetic and the same bits whichever computes what (kerfkite/gpu.hpp, kerfkite/parallel.hpp).
#ifndef KERFKITE_RELIEF_HPP
#define KERFKITE_RELIEF_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {

// One contact job: the cell maxima of the field, row by row, and the rasters to place the cutter on.
struct ReliefContact {
    std::vector<double> maxima;  // rows x cols cells, row-major, each in -depth..0
    int rows = 0, cols = 0;      // cells, 1..1023 each
    int nx = 0, ny = 0;          // segments along each raster, and rasters - 1
    double width = 0, height = 0, depth = 0, diameter = 0, allowance = 0;
    bool ball = false;
};

// The depth at each raster vertex, (ny + 1) rows of nx + 1, and in `ran` what computed them: "CPU, n threads",
// "GPU, device", or "GPU+CPU, ..." when the GPU took the first rasters and the threads the rest. Throws
// ValueError when the work exceeds 2,000,000,000 checks.
std::vector<double> relief_depths(const ReliefContact &job, std::string &ran);

// studio.relief.prepare_relief(data): {"paths", "path_depths", "device", "surface", "warnings"}, the device being
// "cpu", "gpu" or "gpu+cpu". Settings are checked in the reference's order, with its messages; heightmap_rgb text that the
// reference's base64 reader might read differently is declined (NotPorted) rather than guessed.
Json prepare_relief(const Json &data);

// Roughing passes that take the stock down level by level to the roughing cutter's contact: prepare_relief's
// request with "rough_step", the depth of each level, and usually an "allowance" to leave for the finishing pass.
// {"paths", "path_depths", "levels", "step", "lowest", "device", "surface", "warnings"}: the passes level by
// level from the top, each along a raster wherever stock is left below the level above.
Json relief_roughing(const Json &data);

// The GPU the contact uses for large jobs, or why there is none, looking for it if that has not been done.
std::string relief_gpu_status();

// Measures this computer: the contact on all the processor's threads, on the GPU alone where there is one fit to
// use (setting it up if need be), and on both at once, beside all the threads and beside one fewer. From then on
// large jobs of the process go the way the measurements favour, with each side's share set so that both finish
// together; a way is taken only where it is expected to save a twentieth of the time, and the shares follow
// what later jobs measure. Every measured run is compared with the threads' answer bit for bit. Takes about a
// second at most, much less without a GPU. The answer is a report:
//   {"threads", "checks", "threads_ms", "device" (name or null), "device_ms", "device_fixed_ms",
//    "shared": [{"threads", "share", "ms"}], "plan": "threads" | "gpu" | "gpu+cpu", "share", "beside",
//    "seconds", and "device_status" when no device is used}.
// Without a calibration, a large job may go to a discrete GPU alone once the threads have spent as long on large
// jobs as setting it up takes; an integrated GPU, which shares the processor's memory, is used only where a
// calibration found it helps.
Json relief_calibrate();
// The last calibration's report, or null before one.
Json relief_calibration();
// What the calling thread's last contact ran on: "CPU, n threads", "GPU, device" or "none yet".
std::string last_relief_contact();

bool relief_command(const std::string &command);
Json relief_call(const std::string &command, const Json &request);

namespace detail {
// For the self-test, which has no GPU to rely on: puts a stand-in in the GPU's place (on) or restores the GPU
// (off), and forgets every speed and calibration. The stand-in computes contacts with the processor's own
// arithmetic on the thread that drives it, `slowdown` times as slowly, and wrong at item `wrong` when that is
// not negative. A `share` that is not negative is given to every job instead of the one the speeds favour.
void relief_test_device(bool on, double slowdown = 1, long long wrong = -1, double share = -1);
}  // namespace detail

}  // namespace kerfkite

#endif
