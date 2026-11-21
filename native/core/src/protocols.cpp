// Controller profiles and file post-processing: studio/protocols.py.
#include "kerfkite/cam.hpp"

#include <algorithm>
#include <map>

#include "protocols_data.hpp"

namespace kerfkite {

using json = nlohmann::json;

namespace {

const std::map<std::string, Protocol> &table() {
    static const std::map<std::string, Protocol> protocols = [] {
        std::map<std::string, Protocol> rows;
        for (const json &row : json::parse(PROTOCOLS_JSON)) {
            Protocol p;
            p.id = row.at("id").get<std::string>();
            p.name = row.at("name").get<std::string>();
            p.family = row.at("family").get<std::string>();
            p.notes = row.at("notes").get<std::string>();
            p.modes = row.at("modes").get<std::vector<std::string>>();
            rows.emplace(p.id, std::move(p));
        }
        return rows;
    }();
    return protocols;
}

bool is_text(const json *value, const char *text) {
    return value && value->is_string() && value->get_ref<const std::string &>() == text;
}

}  // namespace

// protocol is the project's value, or null when the project has none (the reference then uses grbl).
const Protocol &get_protocol(const json *protocol) {
    if (!protocol) return table().at("grbl");
    if (protocol->is_string()) {
        const auto found = table().find(protocol->get_ref<const std::string &>());
        if (found != table().end()) return found->second;
    }
    throw ValueError("Unsupported controller protocol");
}

std::vector<std::string> postprocess(const std::vector<std::string> &lines, const json *protocol,
                                     const std::string &mode, const json *dwell_units) {
    const Protocol &profile = get_protocol(protocol);
    if (std::find(profile.modes.begin(), profile.modes.end(), mode) == profile.modes.end())
        throw ValueError(profile.name + " dynamic laser export is not implemented; choose a validated laser profile");
    if (profile.family == "grbl") return lines;
    std::vector<std::string> result;
    if (profile.family == "marlin") {
        for (const std::string &line : lines) {
            if (line == "G17" || line == "G94" || line == "G54") continue;
            result.push_back(line == "G4 P2" ? "G4 P2000" : line);
        }
        return result;
    }
    const bool configured_dwell = profile.id == "mach3" || profile.id == "uccnc";
    const bool seconds = is_text(dwell_units, "seconds"), milliseconds = is_text(dwell_units, "milliseconds");
    if (configured_dwell && !seconds && !milliseconds)
        throw ValueError("Select configured dwell units: seconds or milliseconds");
    for (const std::string &line : lines) {
        result.push_back(line == "G4 P2" && configured_dwell && milliseconds ? "G4 P2000" : line);
        if (line == "G90") result.insert(result.end(), {"G40", "G49", "G80", "G61"});
    }
    result.push_back("M30");
    return result;
}

}  // namespace kerfkite
