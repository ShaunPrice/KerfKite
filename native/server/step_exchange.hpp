// STEP exchange: studio/cad_exchange.py's STEP import and export. The server checks the STEP text and builds the
// geometry's description in its own code (the core's "step-part" and "step-faces"); Open CASCADE, which reads,
// tessellates and writes STEP, runs in the separate kerfkite-step add-on (native/step), one process for each
// request, so that its LGPL libraries stay replaceable files beside it. Without the add-on, STEP requests are
// answered with kNoStep.
#ifndef KERFKITE_SERVER_STEP_EXCHANGE_HPP
#define KERFKITE_SERVER_STEP_EXCHANGE_HPP

#include <mutex>
#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

extern const char *const kNoStep;

class StepExchange {
public:
    // The add-on program, or empty for none.
    explicit StepExchange(std::string program);

    bool available() const { return !program_.empty(); }
    const std::string &program() const { return program_; }

    // _decode: the bytes of a STEP file sent as base64 text, up to 1.5 MB. ValueError with the reference's messages.
    static std::string decode(const Json *source);
    // _read_shape's checks before Open CASCADE reads the bytes: an ISO 10303-21 document up to 1.5 MB that is
    // complete and within the entity budget.
    static void check(const std::string &raw);

    // inspect_step(raw) for checked bytes: the tessellated shape, {"vertices", "triangles", "bounds", "area",
    // "faces", "solids", "solidOnly", "volume"}.
    Json inspect(const std::string &raw);
    // write_step: the STEP document of {"faces"} (a surface), {"features"} (a part) or {"source"} (checked bytes as
    // base64 text, read and written again).
    std::string write(const Json &what);

private:
    // One request to the add-on, whose failure answers with the message given.
    Json call(const Json &request, const char *failure);

    std::string program_;
    std::mutex lock_;  // Open CASCADE's exchange settings are the process's: one request at a time, as the reference
};

}  // namespace server
}  // namespace kerfkite

#endif
