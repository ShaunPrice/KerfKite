// The controller protocols. See protocols.hpp.
#include "protocols.hpp"

#include <utility>

#include "kerfkite/errors.hpp"
#include "protocols_data.hpp"

namespace kerfkite {
namespace server {

using json = Json;

const std::vector<Protocol> &protocols() {
    static const std::vector<Protocol> rows = [] {
        std::vector<Protocol> out;
        for (const json &row : json::parse(PROTOCOLS_JSON)) {
            Protocol p;
            p.id = row.at("id").get<std::string>();
            p.name = row.at("name").get<std::string>();
            p.family = row.at("family").get<std::string>();
            p.notes = row.at("notes").get<std::string>();
            p.live_transport = row.at("live_transport").get<bool>();
            for (const json &mode : row.at("modes")) p.modes.push_back(mode.get<std::string>());
            out.push_back(std::move(p));
        }
        return out;
    }();
    return rows;
}

const Protocol &protocol_named(const std::string &name) {
    for (const Protocol &p : protocols())
        if (p.id == name) return p;
    throw ValueError("Unsupported controller protocol");
}

const Protocol &protocol_of(const Json *value) {
    if (!value) return protocol_named("grbl");
    if (!value->is_string()) throw ValueError("Unsupported controller protocol");
    return protocol_named(value->get<std::string>());
}

Json protocols_json() {
    json out = json::array();
    for (const Protocol &p : protocols()) {
        json row = json::object();
        row["id"] = p.id;
        row["name"] = p.name;
        row["live_transport"] = p.live_transport;
        row["family"] = p.family;
        row["notes"] = p.notes;
        json modes = json::array();
        for (const std::string &mode : p.modes) modes.push_back(mode);
        row["modes"] = modes;
        out.push_back(row);
    }
    return out;
}

}  // namespace server
}  // namespace kerfkite
