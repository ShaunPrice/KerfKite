// A small check that needs no Python: number text and one complete program. The full comparison with the
// reference is validation/native_core_parity.py.
#include <cstdio>
#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/number_text.hpp"

using json = nlohmann::json;
using namespace kerfkite;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static std::string fixed(double value, int decimals) {
    std::string out;
    text::fixed(out, value, decimals);
    return out;
}

template <typename Thrown> static std::string message(const char *project) {
    try {
        generate(json::parse(project));
    } catch (const Thrown &error) {
        return error.what();
    } catch (const std::exception &) {
        return "(another exception)";
    }
    return "(nothing thrown)";
}

int main() {
    // Ties round to even on the exact binary value, as Python's formatting does.
    expect(fixed(0.125, 2) == "0.12", "0.125 to two places");
    expect(fixed(0.375, 2) == "0.38", "0.375 to two places");
    expect(fixed(2.5, 0) == "2", "2.5 to no places");
    expect(fixed(-0.0004, 3) == "-0.000", "a small negative keeps its sign");
    expect(fixed(1234.56789, 3) == "1234.568", "1234.56789 to three places");
    expect(fixed(0.1 + 0.2, 9) == "0.300000000", "0.1 + 0.2 to nine places");
    double r = 0;
    expect(text::rounded(2.675, 2, r) && r == 2.67, "round(2.675, 2)");
    expect(text::repr(680.0) == "680.0" && text::repr(0.1) == "0.1" && text::repr(1e16) == "1e+16" &&
               text::repr(0.00001) == "1e-05" && text::repr(-2.5) == "-2.5" && text::repr(123456789.125) == "123456789.125",
           "repr");

    const char *project = R"({"mode":"router","protocol":"grbl","width":100,"height":50,"feed":600,"power":12000,
        "depth":1,"step":0.5,"paths":[[[10,10],[20.5,10],[20.5,30]]]})";
    const Job job = generate(json::parse(project));
    std::string program;
    for (const std::string &line : job.lines) program += line + "\n";
    const std::string expected =
        "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\n"
        "G0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-0.500 F100.0\nG1 X20.500 Y10.000 F600.0\nG1 X20.500 Y30.000 F600.0\nG0 Z5.000\n"
        "G0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-1.000 F100.0\nG1 X20.500 Y10.000 F600.0\nG1 X20.500 Y30.000 F600.0\nG0 Z5.000\n"
        "M5\n";
    expect(program == expected, "a two-level router program");
    expect(job.cut_length == 61.0 && job.segments.size() == 4 && job.warnings.size() == 3, "its summary");
    expect(job.bounds[0] == 10 && job.bounds[1] == 10 && job.bounds[2] == 20.5 && job.bounds[3] == 30, "its bounds");

    expect(message<ValueError>(R"({"mode":"plasma"})") == "Choose router, laser or knife", "an unknown head");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[0,0],[101,0]]]})") == "X must be between 0 and 100.0", "a point outside the work area");
    expect(message<NotPorted>(R"({"mode":"laser","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[0,0],[1,0]]]})") == "laser jobs", "a feature that is not ported is declined, not guessed");

    std::printf(failures ? "%d checks failed\n" : "PASS native core self-test%.0d\n", failures);
    return failures ? 1 : 0;
}
