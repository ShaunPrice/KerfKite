// A small check that needs no Python: number text and one complete program. The full comparison with the
// reference is validation/native_core_parity.py.
#include <cmath>
#include <cstdio>
#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/leveling.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/pcb.hpp"

using json = kerfkite::Json;
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

// What a circuit board command answers, or the message it refuses with.
static json board(const char *command, const std::string &text, json request = json::object()) {
    request["text"] = text;
    try {
        return pcb_call(command, request);
    } catch (const ValueError &error) {
        return std::string(error.what());
    } catch (const std::exception &) {
        return "(another exception)";
    }
}

static void circuit_boards() {
    // Gerber: a flash, a line, a quarter arc and a macro with arithmetic, in millimetres with six decimals.
    const std::string head = "%FSLAX46Y46*%\n%MOMM*%\n";
    const json layer =
        board("gerber",
              head +
                  "%AMBOX*$3=$1x2*21,1,$3,$2,0,0,90*%\n%ADD10C,0.5*%\n%ADD11BOX,1.5X1*%\nD11*\nX2000000Y3000000D03*\n"
                  "D10*\nG01*\nX0Y0D02*\nX10000000Y0D01*\nG75*\nG03X0Y10000000I-10000000J0D01*\nM02*\n");
    expect(layer.is_object() && layer["counts"] == json::parse(R"({"flash":1,"line":1,"arc":1,"region":0})") &&
               layer["warnings"].empty(),
           "a Gerber layer is read");
    if (layer.is_object()) {
        const json &objects = layer["objects"];
        expect(objects[0] == json::parse(R"({"type":"flash","polarity":"dark","aperture":11,"at":[2.0,3.0]})"),
               "its flash");
        expect(objects[1]["end"] == json::parse("[10.0,0.0]") && objects[2]["centre"] == json::parse("[0.0,0.0]") &&
                   objects[2]["clockwise"] == false && objects[2]["points"].size() > 8,
               "its line and arc");
        bool on_arc = true;
        for (const json &point : objects[2]["points"])
            on_arc = on_arc && std::fabs(std::hypot(point[0].get<double>(), point[1].get<double>()) - 10) < 1e-5;
        expect(on_arc, "the flattened arc lies on the circle");
        // $3 = 1.5 x 2 wide and 1 high, turned a quarter turn: 1 wide and 3 high.
        expect(layer["apertures"][1]["parts"][0]["points"] ==
                   json::parse("[[0.5,-1.5],[0.5,1.5],[-0.5,1.5],[-0.5,-1.5],[0.5,-1.5]]"),
               "a macro with arithmetic and rotation");
        expect(layer["bounds"] == json::parse("[-0.25,-0.25,10.25,10.25]"),
               "the bounds include the apertures and the arc's reach");
    }
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\nX0Y0D03*\n") ==
               "The Gerber file has no end-of-file command (M02); it may be truncated",
           "a truncated Gerber file is refused");
    expect(board("gerber", head + "D12*\nM02*\n") == "Line 3: aperture D12 is selected but not defined",
           "an unknown aperture is refused");
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\n%SRX1000Y1000I1J1*%\nX0Y0D03*\nM02*\n") ==
               "Step-and-repeat would draw more than 100000 objects",
           "a huge step-and-repeat is refused before it is expanded");
    expect(
        board("gerber", head + "%AMD*1,1," + std::string(5000, '(') + "1" + std::string(5000, ')') +
                            ",0,0*%\n%ADD10D*%\nM02*\n") == "Line 4: aperture macro D nests brackets more than 32 deep",
        "deep macro nesting is refused");
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\nX99999999999Y0D03*\nM02*\n") ==
               "Line 5: a coordinate lies outside -10000 to 10000 mm",
           "an absurd coordinate is refused");

    // A board outline: four lines out of order close into one path; with one missing it is open.
    const std::string edges = head +
                              "%ADD10C,0.1*%\nD10*\nG01*\nX0Y0D02*\nX8000000Y0D01*\nX0Y5000000D02*\nX0Y0D01*"
                              "\nX8000000Y5000000D02*\nX8000000Y0D01*\n";
    const json outline =
        board("gerber-outline", edges + "X8000000Y5000000D02*\nX0Y5000000D01*\nM02*\n", json{{"offset", {1, 2}}});
    expect(outline.is_object() &&
               outline["paths"] == json::parse("[[[1.0,2.0],[9.0,2.0],[9.0,7.0],[1.0,7.0],[1.0,2.0]]]") &&
               outline["bounds"] == json::parse("[1.0,2.0,9.0,7.0]"),
           "a board outline is joined into a closed path");
    const json open = board("gerber-outline", edges + "M02*\n");
    expect(open.is_string() && open.get<std::string>().find("The board outline is open at X") == 0,
           "an open outline is refused");

    // Excellon: inches with leading zeros omitted and a repeat; then a file that leaves its format unstated.
    const json drill = board("excellon", "M48\nINCH,TZ\nT1C0.04\n%\nT1\nX10000Y5000\nR2X2500\nM30\n");
    expect(drill.is_object() &&
               drill["tools"] ==
                   json::parse(R"([{"number":1,"diameter":1.016,"hits":[[25.4,12.7],[31.75,12.7],[38.1,12.7]]}])") &&
               drill["interpretation"]["omitted_zeros"] == "leading" && drill["warnings"].size() == 1,
           "a drill file in inches with a repeat");
    const std::string bare = "M48\nM72\nT1C0.04\n%\nT1\nX5200Y3400\nM30\n";
    const json guessed = board("excellon", bare),
               stated = board("excellon", bare, json{{"omitted_zeros", "leading"}, {"integer", 2}, {"decimal", 4}});
    expect(guessed.is_object() && guessed["warnings"].size() == 2 &&
               guessed["interpretation"]["zeros_from"] == "default" &&
               guessed["tools"][0]["hits"] == json::parse("[[1320.8,863.6]]"),
           "an unstated number format is read by the defaults and reported");
    expect(stated.is_object() && stated["warnings"].empty() &&
               stated["tools"][0]["hits"] == json::parse("[[13.208,8.636]]"),
           "the request can state the number format");
    expect(board("excellon", "M48\nT1C0.8\n%\nT1\nX1.0Y1.0\nM30\n") ==
               "The drill file does not state its units (METRIC, INCH, M71 or M72); give \"units\" in the request",
           "units are not assumed");
    const json drills = board(
        "excellon-drills", "M48\nMETRIC\nT1C0.8\nT2C0.4\nT3C0.8\n%\nT1\nX1.0Y1.0\nT2\nX2.0Y1.0\nT3\nX3.0Y1.0\nM30\n");
    expect(drills.is_object() && drills["operations"] == json::parse(R"([
               {"operation":"drill","tool_shape":"drill","tool_diameter":0.4,"paths":[[[2.0,1.0],[2.0,1.0]]]},
               {"operation":"drill","tool_shape":"drill","tool_diameter":0.8,"paths":[[[1.0,1.0],[1.0,1.0]],[[3.0,1.0],[3.0,1.0]]]}])"),
           "holes become drilling operations grouped by diameter");
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
        "paths":[[[9,9],[9.00000009,9],[9.00000018,9],[9,9]]]})") ==
               "a drag-knife path that returns to a point (the reference fails on it)",
           "a case that is not reproduced is declined, not guessed");

    // Drilling: a peck at each marker, with the last peck at the full depth.
    const Job drilled = generate(json::parse(R"({"mode":"router","operation":"drill","protocol":"grbl","width":100,
        "height":50,"feed":600,"power":12000,"depth":1,"drill_peck":0.4,"paths":[[[10,10],[10,10]],[[30.5,20],[30.5,20]]]})"));
    program.clear();
    for (const std::string &line : drilled.lines) program += line + "\n";
    const std::string pecks = "G1 Z-0.400 F100.0\nG0 Z5.000\nG1 Z-0.800 F100.0\nG0 Z5.000\nG1 Z-1.000 F100.0\nG0 Z5.000\n";
    expect(program == "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 X10.000 Y10.000\n" + pecks +
                          "G0 X30.500 Y20.000\n" + pecks + "M5\n",
           "a peck-drilling program");
    expect(drilled.cut_length == 34.4 && drilled.cut_minutes == 0.34 && drilled.segments.size() == 6 &&
               drilled.segments[0].end_z == -0.4 && drilled.segments[0].start_z == 5 && drilled.warnings.size() == 2 &&
               drilled.bounds[0] == 10 && drilled.bounds[1] == 10 && drilled.bounds[2] == 30.5 && drilled.bounds[3] == 20,
           "its summary");
    expect(message<ValueError>(R"({"mode":"router","operation":"drill","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[10,10],[11,10]]]})") == "Drill requires point markers or closed circular hole outlines",
           "a line is not a hole");

    // A drag knife: the blade trails its pivot, so the path starts with a lead-in and swivels at the corner.
    const Job sliced = generate(json::parse(R"({"mode":"knife","protocol":"grbl","width":100,"height":50,"feed":600,
        "power":1,"depth":0.5,"paths":[[[10,10],[20,10],[20,20]]]})"));
    const std::vector<std::string> &cut = sliced.lines;
    expect(cut.size() == 33 && cut[6] == "G0 Z5.000" && cut[7] == "G0 Z5.000" && cut[8] == "G0 X9.250 Y10.000" &&
               cut[9] == "G1 Z-0.500 F100.0" && cut[10] == "G1 X10.250 Y10.000 F600.0" &&
               cut[11] == "G1 X20.250 Y10.000 F600.0" && cut[12] == "G1 X20.249 Y10.022 F600.0" &&
               cut[29] == "G1 X20.000 Y10.250 F600.0" && cut[30] == "G1 X20.000 Y20.250 F600.0" && cut[32] == "M5",
           "a drag-knife program");
    expect(sliced.cut_length == 21.39 && sliced.warnings.size() == 4 && sliced.bounds[0] == 9.25 &&
               sliced.bounds[3] == 20.25,
           "its summary");
    expect(message<ValueError>(R"({"mode":"knife","width":100,"height":50,"feed":600,"power":1,"knife_offset":2,
        "paths":[[[10,10],[20,10]]]})") == "knife_lead must be between 2.0 and 20", "a lead shorter than the offset");

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

    circuit_boards();

    std::printf(failures ? "%d checks failed\n" : "PASS native core self-test%.0d\n", failures);
    return failures ? 1 : 0;
}
