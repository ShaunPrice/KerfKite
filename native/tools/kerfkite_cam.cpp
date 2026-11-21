// kerfkite-cam: G-code from a Studio project without Python.
//
//   kerfkite-cam generate PROJECT.json [-o PROGRAM.nc] [--json]
//   kerfkite-cam probe-plan REQUEST.json     a probing program for the cutting bounds of a job
//   kerfkite-cam height-map REQUEST.json     the surface heights from the probe results
//   kerfkite-cam level REQUEST.json          a program corrected to follow that surface
//   kerfkite-cam batch CASES.jsonl
//   kerfkite-cam numbers
//   kerfkite-cam arithmetic
//
// generate writes the program (or, with --json, the whole job) and exits 0. Exit 2 means the project was
// rejected, with the reference's message on standard error; exit 3 means it needs a feature that is not
// ported yet; exit 1 is a usage or file error.
//
// batch and numbers serve validation/native_core_parity.py. batch reads one {"project": ...} object per
// line and answers each with {"ok": job}, {"error": message} or {"declined": feature}. numbers reads
// "HEXFLOAT DECIMALS" lines and writes the fixed-point text, round() and repr() of each value. arithmetic
// answers "dist" and "sum" requests with the core's versions of Python's math.dist and sum().
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/leveling.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/py_math.hpp"

using json = nlohmann::json;

namespace {

int usage() {
    std::cerr << "usage: kerfkite-cam generate PROJECT.json [-o PROGRAM.nc] [--json]\n"
                 "       kerfkite-cam probe-plan|height-map|level REQUEST.json\n"
                 "       kerfkite-cam batch CASES.jsonl\n"
                 "       kerfkite-cam numbers < VALUES\n";
    return 1;
}

bool read(const std::string &path, std::string &out) {
    std::ostringstream buffer;
    if (path == "-") {
        buffer << std::cin.rdbuf();
    } else {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        buffer << file.rdbuf();
    }
    out = buffer.str();
    return true;
}

int generate(int argc, char **argv) {
    std::string source, target;
    bool whole = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) target = argv[++i];
        else if (arg == "--json") whole = true;
        else if (source.empty()) source = arg;
        else return usage();
    }
    std::string text;
    if (source.empty()) return usage();
    if (!read(source, text)) {
        std::cerr << "kerfkite-cam: cannot read " << source << "\n";
        return 1;
    }
    try {
        const kerfkite::Job job = kerfkite::generate(json::parse(text));
        std::string out;
        if (whole) {
            out = kerfkite::to_json(job).dump() + "\n";
        } else {
            for (const std::string &line : job.lines) out.append(line).push_back('\n');
        }
        if (target.empty()) {
            std::fwrite(out.data(), 1, out.size(), stdout);
        } else {
            std::ofstream file(target, std::ios::binary);
            file.write(out.data(), static_cast<std::streamsize>(out.size()));
            if (!file) {
                std::cerr << "kerfkite-cam: cannot write " << target << "\n";
                return 1;
            }
        }
        if (!whole)
            for (const std::string &warning : job.warnings) std::cerr << "note: " << warning << "\n";
        return 0;
    } catch (const kerfkite::ValueError &error) {
        std::cerr << error.what() << "\n";
        return 2;
    } catch (const kerfkite::NotPorted &error) {
        std::cerr << "Not available in the native core yet: " << error.what() << "\n";
        return 3;
    } catch (const json::exception &error) {
        std::cerr << "kerfkite-cam: the project is not valid JSON: " << error.what() << "\n";
        return 1;
    }
}

// A levelling command: the request is a JSON file (or - for standard input), the answer JSON on standard output.
int request(const std::string &command, const std::string &path) {
    std::string text;
    if (!read(path, text)) {
        std::cerr << "kerfkite-cam: cannot read " << path << "\n";
        return 1;
    }
    try {
        std::cout << kerfkite::call(command, json::parse(text)).dump() << "\n";
        return 0;
    } catch (const kerfkite::ValueError &error) {
        std::cerr << error.what() << "\n";
        return 2;
    } catch (const json::exception &error) {
        std::cerr << "kerfkite-cam: the request is not valid JSON: " << error.what() << "\n";
        return 1;
    }
}

int batch(const std::string &path) {
    std::ifstream cases(path, std::ios::binary);
    if (!cases) {
        std::cerr << "kerfkite-cam: cannot read " << path << "\n";
        return 1;
    }
    std::string line;
    while (std::getline(cases, line)) {
        if (line.empty()) continue;
        json answer;
        try {
            answer = json{{"ok", kerfkite::to_json(kerfkite::generate(json::parse(line).at("project")))}};
        } catch (const kerfkite::ValueError &error) {
            answer = json{{"error", error.what()}};
        } catch (const kerfkite::NotPorted &error) {
            answer = json{{"declined", error.what()}};
        } catch (const std::exception &error) {
            answer = json{{"invalid", error.what()}};
        }
        std::cout << answer.dump() << "\n";
    }
    return 0;
}

int numbers() {
    std::string hex;
    int decimals;
    while (std::cin >> hex >> decimals) {
        const double value = std::strtod(hex.c_str(), nullptr);
        std::string fixed;
        if (!kerfkite::text::fixed(fixed, value, decimals)) fixed = "-";
        double rounded;
        char round_text[40] = "-";
        if (kerfkite::text::rounded(value, decimals, rounded))
            std::snprintf(round_text, sizeof round_text, "%a", rounded);
        std::cout << fixed << '\t' << round_text << '\t' << kerfkite::text::repr(value) << '\n';
    }
    return 0;
}

// "dist AX AY BX BY" and "sum N V1 .. VN", all in hexadecimal float form; the answer is written the same way.
int arithmetic() {
    std::string word;
    while (std::cin >> word) {
        double answer = 0;
        const auto value = [] {
            std::string hex;
            std::cin >> hex;
            return std::strtod(hex.c_str(), nullptr);
        };
        if (word == "dist") {
            const double ax = value(), ay = value(), bx = value(), by = value();
            answer = kerfkite::pymath::dist(ax, ay, bx, by);
        } else if (word == "sum") {
            int n = 0;
            std::cin >> n;
            std::vector<double> values;
            for (int i = 0; i < n; ++i) values.push_back(value());
            answer = kerfkite::pymath::sum(values.begin(), values.end());
        } else {
            return usage();
        }
        char text[40];
        std::snprintf(text, sizeof text, "%a", answer);
        std::cout << text << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    if (command == "generate") return generate(argc, argv);
    if (command == "batch" && argc == 3) return batch(argv[2]);
    if ((command == "probe-plan" || command == "height-map" || command == "level") && argc == 3) return request(command, argv[2]);
    if (command == "numbers" && argc == 2) return numbers();
    if (command == "arithmetic" && argc == 2) return arithmetic();
    return usage();
}
