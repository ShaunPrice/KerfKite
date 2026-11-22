// Relief finishing from a height field without Python: studio/relief.py's prepare_relief, to the bit.
//
// A height field (heights in 0..1, or the RGB bytes of an image) is sampled on rasters across the work, and at
// each raster point the cutter (ball or flat) is placed as deep as it may go without entering the field, each
// cell of which counts as its highest corner. The result is paths and depths for studio.cam, and a record of
// the surface. The contact runs on the processor's threads, or on a discrete GPU for large jobs, with the same
// arithmetic and the same bits either way (kerfkite/gpu.hpp, kerfkite/parallel.hpp).
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

// The depth at each raster vertex, (ny + 1) rows of nx + 1, and in `ran` what computed them: "CPU, n threads"
// or "GPU, device". Throws ValueError when the work exceeds 2,000,000,000 checks.
std::vector<double> relief_depths(const ReliefContact &job, std::string &ran);

// studio.relief.prepare_relief(data): {"paths", "path_depths", "device", "surface", "warnings"}, the device being
// "cpu" or "gpu". Settings are checked in the reference's order, with its messages; heightmap_rgb text that the
// reference's base64 reader might read differently is declined (NotPorted) rather than guessed.
Json prepare_relief(const Json &data);

// The GPU the contact uses for large jobs, or why there is none, looking for it if that has not been done.
std::string relief_gpu_status();
// What the calling thread's last contact ran on: "CPU, n threads", "GPU, device" or "none yet".
std::string last_relief_contact();

bool relief_command(const std::string &command);
Json relief_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
