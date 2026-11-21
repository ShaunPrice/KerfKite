// A small check that needs no Python: number text and one complete program. The full comparison with the
// reference is validation/native_core_parity.py.
#include <cmath>
#include <cstdio>
#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/leveling.hpp"
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
    std::string untouched = "x";
    expect(!text::fixed(untouched, 1.0, 10) && !text::fixed(untouched, 1.0, -1) && untouched == "x" && !text::rounded(1.0, 4, r),
           "places outside the supported range are refused");
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
    expect(message<NotPorted>(R"({"mode":"knife","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[0,0],[1,0]]]})") == "drag-knife paths", "a feature that is not ported is declined, not guessed");

    // A ramp entry: one lap down a closed contour at no more than the entry angle, then a lap at depth.
    const Job ramp = generate(json::parse(R"({"mode":"router","width":100,"height":50,"feed":600,"power":12000,
        "depth":1,"step":1,"router_entry":"ramp","entry_angle":15,"paths":[[[10,10],[30,10],[30,20],[10,10]]]})"));
    program.clear();
    for (const std::string &line : ramp.lines) program += line + "\n";
    expect(program ==
               "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 Z5.000\nG0 X10.000 Y10.000\n"
               "G1 X10.000 Y10.000 Z0.000 F100.0\nG1 X30.000 Y10.000 Z-0.382 F100.0\nG1 X30.000 Y20.000 Z-0.573 F100.0\n"
               "G1 X10.000 Y10.000 Z-1.000 F100.0\nG1 X30.000 Y10.000 Z-1.000 F600.0\nG1 X30.000 Y20.000 Z-1.000 F600.0\n"
               "G1 X10.000 Y10.000 Z-1.000 F600.0\nG0 Z5.000\nM5\n",
           "a ramp entry program");
    expect(ramp.cut_length == 109.73 && ramp.cut_minutes == 0.66 && ramp.warnings.size() == 2, "its summary");
    expect(to_json(ramp)["segments"][0].dump() == "[10.0,10.0,10.0,10.0,0,null,5.0]",
           "the first height of a ramp is the reference's integer 0");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"router_entry":"ramp",
        "paths":[[[10,10],[30,30],[30,10],[10,30],[10,10]]]})") ==
               "Repair crossing or degenerate machining contours before using a ramp",
           "a ramp along a contour that crosses itself");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"router_entry":"helix",
        "paths":[[[10,10],[30,10],[30,20],[10,10]]]})") == "Helical entry requires a pocket with confirmed cutter clearance",
           "a helix on a contour");

    // Holding tabs: the contour rises to the tab height over each tab, with vertical moves at its shoulders.
    const Job tabbed = generate(json::parse(R"({"mode":"router","width":100,"height":50,"feed":600,"power":12000,
        "depth":2,"step":2,"stock_thickness":2,"tabs":{"width":2,"height":0.5,"count":1},
        "paths":[[[10,10],[30,10],[30,20],[10,20],[10,10]]]})"));
    program.clear();
    for (const std::string &line : tabbed.lines) program += line + "\n";
    expect(program ==
               "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-2.000 F100.0\n"
               "G1 X30.000 Y10.000 F100.0 Z-2.000\nG1 X30.000 Y17.413 F100.0 Z-2.000\nG1 X30.000 Y17.413 F100.0 Z-1.500\n"
               "G1 X30.000 Y20.000 F100.0 Z-1.500\nG1 X27.413 Y20.000 F100.0 Z-1.500\nG1 X27.413 Y20.000 F100.0 Z-2.000\n"
               "G1 X10.000 Y20.000 F100.0 Z-2.000\nG1 X10.000 Y10.000 F100.0 Z-2.000\nG0 Z5.000\nM5\n",
           "a program with one holding tab");
    expect(tabbed.cut_length == 61.0 && tabbed.cut_minutes == 0.61 && tabbed.warnings.size() == 6 && tabbed.segments.size() == 8,
           "its summary");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"depth":2,"stock_thickness":2,
        "tabs":{"width":40,"height":0.5,"count":2},"paths":[[[10,10],[30,10],[30,20],[10,20],[10,10]]]})") ==
               "Tabs overlap or cover the contour; reduce count/width or change positions",
           "tabs that cover the contour");

    // Probe levelling: a plan over the job, a tilted plane as the probed surface, and the job following it.
    const json plan = probe_plan(json{{"bounds", job.bounds}, {"width", 100}, {"height", 50}, {"zero", true}});
    expect(plan["grid"] == json::parse(R"({"x0":8.0,"y0":8.0,"dx":7.25,"dy":8.0,"columns":3,"rows":4})"), "the probe grid");
    expect(plan["lines"].size() == 4 + 3 * 12 + 1 && plan["lines"][2] == "M5" && plan["lines"][4] == "G0 X8.000 Y8.000" &&
               plan["lines"][5] == "G38.2 Z-1.000 F25.0" && plan["lines"][6] == "G10 L20 P1 Z0.000" &&
               plan["lines"][8] == "G0 X15.250 Y8.000" && plan["order"][3] == json::parse("[2,1]"),
           "the probing program");
    json reports = json::array();
    for (const json &point : plan["points"]) {
        std::string z;
        text::fixed(z, -37.5 + 0.01 * point[0].get<double>(), 3);
        reports.push_back("[PRB:0.000,0.000," + z + ":1]");
    }
    const HeightMap surface = height_map_from_probes(json{{"grid", plan["grid"]}, {"order", plan["order"]}, {"reports", reports}});
    expect(surface.offsets[0] == 0 && std::fabs(surface.offsets[5] - 0.145) < 1e-12 && std::fabs(surface.highest() - 0.145) < 1e-12,
           "the height map is relative to the first point");
    const std::vector<std::string> followed = level_program(job.lines, surface, 5);
    const std::vector<std::string> first_pass(followed.begin() + 9, followed.begin() + 20);
    const std::vector<std::string> wanted = {
        "G0 Z5.000", "G0 X10.000 Y10.000", "G1 Z-0.480 F100.0",
        "G1 X13.500 Y10.000 F600.0 Z-0.445", "G1 X17.000 Y10.000 F600.0 Z-0.410", "G1 X20.500 Y10.000 F600.0 Z-0.375",
        "G1 X20.500 Y15.000 F600.0 Z-0.375", "G1 X20.500 Y20.000 F600.0 Z-0.375", "G1 X20.500 Y25.000 F600.0 Z-0.375",
        "G1 X20.500 Y30.000 F600.0 Z-0.375", "G0 Z5.000"};
    expect(first_pass == wanted && followed.size() == job.lines.size() + 10, "the first pass follows the surface");
    try {
        level_program({"G0 Z5.000", "G0 X10.000 Y10.000", "G2 X20.000 Y10.000 I5 J0 F600"}, surface, 5);
        expect(false, "an arc is refused");
    } catch (const ValueError &error) {
        expect(std::string(error.what()).find("only straight moves") != std::string::npos, "an arc is refused");
    }

    std::printf(failures ? "%d checks failed\n" : "PASS native core self-test%.0d\n", failures);
    return failures ? 1 : 0;
}
