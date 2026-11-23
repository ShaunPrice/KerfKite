// The controller protocols of studio/protocols.py, from the table shared with the editor
// (studio/static/protocols.json, built into the program).
#ifndef KERFKITE_SERVER_PROTOCOLS_HPP
#define KERFKITE_SERVER_PROTOCOLS_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

struct Protocol {
    std::string id, name, family, notes;
    bool live_transport = false;
    std::vector<std::string> modes;
};

// In the table's order.
const std::vector<Protocol> &protocols();
// get_protocol(name); ValueError("Unsupported controller protocol") for an unknown one.
const Protocol &protocol_named(const std::string &name);
// get_protocol(value) of a JSON value, grbl when it is absent (null pointer).
const Protocol &protocol_of(const Json *value);
// The /api/protocols answer: each protocol as asdict gives it.
Json protocols_json();

}  // namespace server
}  // namespace kerfkite

#endif
