// kerfkite-cam: G-code from a Studio project without Python.
//
//   kerfkite-cam generate PROJECT.json [-o PROGRAM.nc] [--json]
//   kerfkite-cam batch CASES.jsonl
//   kerfkite-cam numbers
//
// generate writes the program (or, with --json, the whole job) and exits 0. Exit 2 means the project was
// rejected, with the reference's message on standard error; exit 3 means it needs a feature that is not
// ported yet; exit 1 is a usage or file error.
//
// batch and numbers serve validation/native_core_parity.py. batch reads one {"project": ...} object per
// line and answers each with {"ok": job}, {"error": message} or {"declined": feature}. numbers reads
// "HEXFLOAT DECIMALS" lines and writes the fixed-point text, round() and repr() of each value.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/number_text.hpp"

using json = nlohmann::json;

namespace {

int usage() {
    std::cerr << "usage: kerfkite-cam generate PROJECT.json [-o PROGRAM.nc] [--json]\n"
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
        if (decimals <= 3 && kerfkite::text::rounded(value, decimals, rounded))
            std::snprintf(round_text, sizeof round_text, "%a", rounded);
        std::cout << fixed << '\t' << round_text << '\t' << kerfkite::text::repr(value) << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    if (command == "generate") return generate(argc, argv);
    if (command == "batch" && argc == 3) return batch(argv[2]);
    if (command == "numbers" && argc == 2) return numbers();
    return usage();
}
