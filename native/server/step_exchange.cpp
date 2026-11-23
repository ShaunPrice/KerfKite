// STEP exchange through the Open CASCADE add-on. See step_exchange.hpp.
#include "step_exchange.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>
#include <vector>

#include "base64.hpp"
#include "child_process.hpp"
#include "kerfkite/errors.hpp"

namespace kerfkite {
namespace server {

const char *const kNoStep = "STEP exchange needs Open CASCADE, which this build of KerfKite Studio does not include";

namespace {

const size_t kMaxStepBytes = 1500000;
const double kSeconds = 300;  // a part's Booleans can take a while; a request that takes longer is ended

std::string encode_base64(const std::string &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// len(text) of a Python string: its code points, the bytes that do not continue a UTF-8 sequence.
size_t code_points(const std::string &text) {
    size_t n = 0;
    for (const unsigned char c : text) n += (c & 0xC0) != 0x80;
    return n;
}

// bytes.isspace() for one byte, as bytes.lstrip() strips them.
bool ascii_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c'; }

std::string ascii_upper(std::string text) {
    for (char &c : text)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}

}  // namespace

StepExchange::StepExchange(std::string program) : program_(std::move(program)) {}

std::string StepExchange::decode(const Json *source) {
    if (!source || !source->is_string() || code_points(source->get_ref<const std::string &>()) > 4 * ((kMaxStepBytes + 2) / 3))
        throw ValueError("Select a STEP file up to 1.5 MB");
    std::vector<unsigned char> bytes;
    if (!detail::base64(source->get_ref<const std::string &>(), bytes)) throw ValueError("Invalid STEP file encoding");
    return std::string(bytes.begin(), bytes.end());
}

void StepExchange::check(const std::string &raw) {
    size_t start = 0;
    while (start < raw.size() && ascii_space(static_cast<unsigned char>(raw[start]))) ++start;
    static const std::string header = "ISO-10303-21;";
    if (raw.size() > kMaxStepBytes || ascii_upper(raw.substr(start, header.size())) != header)
        throw ValueError("Supply an ISO 10303-21 STEP file up to 1.5 MB");
    const std::string tail = ascii_upper(raw.size() > 100 ? raw.substr(raw.size() - 100) : raw);
    if (tail.find("END-ISO-10303-21;") == std::string::npos || std::count(raw.begin(), raw.end(), '=') > 35000)
        throw ValueError("Incomplete STEP document or STEP entity budget exceeded");
}

Json StepExchange::call(const Json &request, const char *failure) {
    if (!available()) throw ValueError(kNoStep);
    std::lock_guard<std::mutex> hold(lock_);
    std::unique_ptr<ChildProcess> child;
    try {
        child = std::make_unique<ChildProcess>(program_, std::vector<std::string>{});
    } catch (const std::exception &) {
        throw ValueError("The STEP add-on (kerfkite-step) could not be started. Reinstall KerfKite Studio with STEP exchange.");
    }
    std::string line;
    const bool sent = child->write(request.dump());
    child->close_input();
    if (!sent || !child->readline(line, kSeconds)) {
        child->kill();
        throw ValueError(failure);
    }
    Json answer;
    try {
        answer = Json::parse(line);
    } catch (const std::exception &) {
        throw ValueError(failure);
    }
    if (!answer.is_object()) throw ValueError(failure);
    if (answer.contains("error")) {
        const Json &error = answer.at("error");
        throw ValueError(error.is_string() ? error.get<std::string>() : std::string(failure));
    }
    return answer;
}

Json StepExchange::inspect(const std::string &raw) {
    Json request = Json::object();
    request["inspect"] = encode_base64(raw);
    return call(request, "Open CASCADE could not translate this STEP shape");
}

std::string StepExchange::write(const Json &what) {
    Json request = Json::object();
    request["export"] = what;
    const Json answer = call(request, "Could not translate the geometry to STEP");
    std::vector<unsigned char> bytes;
    if (!answer.contains("step") || !answer.at("step").is_string() || !detail::base64(answer.at("step").get<std::string>(), bytes))
        throw ValueError("Open CASCADE could not write the STEP document");
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace server
}  // namespace kerfkite
