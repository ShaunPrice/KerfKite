// The stock-removal preview without Python: studio/simulation.py's simulate, to the bit.
//
// A square grid of n x n points over the stock records the lowest height a cutter (flat, ball, V or drill)
// reaches at each point as it follows each preview segment of a job. It is a sampled picture for the editor,
// not a collision check. Grid rows are shared out between threads; each cell takes its minima in the same order
// on any number of threads.
#ifndef KERFKITE_STOCK_HPP
#define KERFKITE_STOCK_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {

// studio.simulation.simulate(data): {"heights", "x", "y", "width", "height", "stock_thickness", "resolution",
// "warnings"}. When rows is given, it holds the segments as seven doubles each (x, y, nx, ny, z, power, start z),
// as packed preview rows do, and data's "segments" is not read. A segment value that Python's float() would
// refuse with a message naming the text is declined (NotPorted).
Json simulate_stock(const Json &data, const std::vector<double> *rows = nullptr);

bool stock_command(const std::string &command);
Json stock_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
