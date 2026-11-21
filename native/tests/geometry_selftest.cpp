// A check of the geometry layer and the polygon operations that needs no Python. It shows that GEOS is
// linked and answers; whether the answers match the reference to the byte is the business of
// validation/native_core_parity.py. In a build without GEOS it checks that the operations are declined.
#include <cmath>
#include <cstdio>
#include <string>

#include "../core/src/geometry.hpp"
#include "kerfkite/cam.hpp"

using json = nlohmann::json;
using namespace kerfkite;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static bool close_to(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

static const char *square_job(const char *operation) {
    static std::string text;
    text = std::string(R"({"mode":"router","protocol":"grbl","width":100,"height":50,"feed":600,"power":12000,
        "depth":1,"step":1,"tool_diameter":2,"stepover":0.5,"operation":")") +
           operation + R"(","paths":[[[10,10],[20,10],[20,20],[10,20],[10,10]]]})";
    return text.c_str();
}

int main() {
    if (!geometry::available()) {
        expect(geometry::version().empty(), "no GEOS version without GEOS");
        try {
            generate(json::parse(square_job("outside")));
            expect(false, "an offset is declined without GEOS");
        } catch (const NotPorted &error) {
            expect(std::string(error.what()) == "offset, pocket and fill operations", "the decline names the operations");
        }
        std::printf(failures ? "geometry self-test FAILED\n" : "geometry self-test passed (built without GEOS: operations declined)\n");
        return failures ? 1 : 0;
    }

    expect(geometry::version() == "3.14.1", "the GEOS release is the one the reference uses, found " + geometry::version());
    {
        geometry::Context context;
        const geometry::Geometry square = context.polygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}});  // closed for us
        expect(square.kind() == geometry::Kind::Polygon && square.valid() && !square.empty(), "a square is a valid polygon");
        expect(square.area() == 100 && square.exterior().size() == 5, "a square of side 10");
        const geometry::Geometry bow = context.polygon({{0, 0}, {10, 10}, {10, 0}, {0, 10}, {0, 0}});
        expect(!bow.valid(), "a bow-tie is invalid");

        // A buffer with round corners: the square, four side strips and a full circle of the radius.
        const geometry::Geometry grown = context.buffer(square, 2.0);
        expect(close_to(grown.area(), 100 + 4 * 20 + 3.141592653589793 * 4, 0.05), "area of a buffered square");
        const std::array<double, 4> box = grown.bounds();
        expect(close_to(box[0], -2) && close_to(box[1], -2) && close_to(box[2], 12) && close_to(box[3], 12), "bounds of a buffered square");
        expect(context.buffer(square, -6.0).empty(), "a square shrunk past its centre is empty");

        // A frame and a small separate square: the small polygon comes first, and the frame's hole before its outside.
        const geometry::Geometry hole = context.polygon({{3, 3}, {7, 3}, {7, 7}, {3, 7}, {3, 3}});
        const geometry::Geometry frame = context.difference(square, hole);
        const geometry::Geometry dot = context.polygon({{20, 0}, {21, 0}, {21, 1}, {20, 1}, {20, 0}});
        const std::vector<geometry::Ring> rings = geometry::outlines(context.unite(frame, dot));
        expect(rings.size() == 3, "three rings in a frame and a dot");
        if (rings.size() == 3) {
            const auto width = [](const geometry::Ring &ring) {
                double low = ring[0][0], high = ring[0][0];
                for (const geometry::Point &point : ring) low = std::fmin(low, point[0]), high = std::fmax(high, point[0]);
                return high - low;
            };
            expect(close_to(width(rings[0]), 1) && close_to(width(rings[1]), 4) && close_to(width(rings[2]), 10), "rings ordered: small polygon, then hole, then outside");
        }
        expect(close_to(context.symmetric_difference(square, hole).area(), 84) && close_to(context.intersection(square, hole).area(), 16), "set operations");

        // A track with a round aperture, and dark and clear artwork.
        const geometry::Geometry track = context.buffer(context.line({{0, 0}, {10, 0}}), 1.0);
        expect(close_to(track.area(), 20 + 3.141592653589793, 0.01), "a stroked track with round ends");
        geometry::BufferStyle flat;
        flat.cap = geometry::Cap::Flat;
        expect(close_to(context.buffer(context.line({{0, 0}, {10, 0}}), 1.0, flat).area(), 20), "a stroked track with flat ends");
        geometry::Artwork artwork(context);
        artwork.dark(context.polygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
        artwork.dark(context.polygon({{2, 0}, {6, 0}, {6, 4}, {2, 4}, {2, 0}}));
        artwork.clear(context.polygon({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
        artwork.dark(context.polygon({{10, 0}, {11, 0}, {11, 1}, {10, 1}, {10, 0}}));
        const geometry::Geometry image = artwork.image();
        expect(close_to(image.area(), 24 - 1 + 1) && geometry::polygons(image).size() == 2, "dark and clear artwork");

        const geometry::Geometry turned = context.affine(square, 0, -1, 1, 0, 5, 0);  // a quarter turn, then 5 along X
        const std::array<double, 4> moved = turned.bounds();
        expect(moved[0] == -5 && moved[1] == 0 && moved[2] == 5 && moved[3] == 10 && turned.area() == 100, "an affine copy");
    }

    // The operations through the public entry point.
    const Job outside = generate(json::parse(square_job("outside")));
    expect(close_to(outside.bounds[0], 9) && close_to(outside.bounds[1], 9) && close_to(outside.bounds[2], 21) && close_to(outside.bounds[3], 21),
           "an outside offset of a square by a 2 mm cutter");
    expect(outside.warnings.size() == 4 && outside.warnings[1].rfind("Compensation follows even-odd regions", 0) == 0, "the offset's warning follows the controller's note");
    // Four sides and four corners of 32 segments each.
    expect(outside.segments.size() == 4 + 4 * 32, "vertices of a rounded offset, found " + std::to_string(outside.segments.size()));
    const Job inside = generate(json::parse(square_job("inside")));
    expect(inside.segments.size() == 4 && close_to(inside.bounds[0], 11) && close_to(inside.bounds[3], 19), "an inside offset keeps sharp corners");
    const Job pocket = generate(json::parse(square_job("pocket")));
    // Insets at 1, 1.5 ... 4.5 mm from the edge: eight squares.
    expect(pocket.segments.size() == 8 * 4 && pocket.warnings.size() == 5, "a concentric pocket, found " + std::to_string(pocket.segments.size()));
    try {
        std::string text = square_job("inside");
        text.replace(text.find("\"tool_diameter\":2"), 17, "\"tool_diameter\":12");
        generate(json::parse(text));
        expect(false, "a cutter wider than the contour is refused");
    } catch (const ValueError &error) {
        expect(std::string(error.what()) == "Tool does not fit inside these contours", "the refusal is the reference's");
    }
    const Job fill = generate(json::parse(R"({"mode":"laser","protocol":"grbl","width":100,"height":50,"feed":600,"power":800,
        "operation":"fill","interval":2,"paths":[[[10,10],[20,10],[20,20],[10,20],[10,10]]]})"));
    // Rows at y = 10, 12 ... 20, one move each across the square.
    bool rows = fill.segments.size() == 6;
    for (size_t i = 0; rows && i < 6; ++i) {
        const Segment &row = fill.segments[i];
        rows = row.y == 10 + 2.0 * i && row.ny == row.y && std::fabs(row.nx - row.x) == 10;
    }
    expect(rows, "a hatch fill of a square");

    std::printf(failures ? "geometry self-test FAILED\n" : "geometry self-test passed (GEOS %s)\n", geometry::version().c_str());
    return failures ? 1 : 0;
}
