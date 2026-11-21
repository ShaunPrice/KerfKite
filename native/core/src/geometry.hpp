// Planar geometry for the core: a thin layer over the GEOS C API. Internal to the core.
//
// The Python reference does its polygon work through Shapely, which calls GEOS. Each function here makes
// the GEOS call that the matching Shapely call makes, with the same arguments, so that a result has the
// same vertices in the same order as the reference's. The comment on each function names the Shapely call
// and the GEOS function. Nothing here simplifies, snaps or reorders a result.
//
// Use
//
//     geometry::Context context;                              // one per job; not shared between threads
//     geometry::Geometry square = context.polygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
//     geometry::Geometry grown = context.buffer(square, 1.5);  // round joins, 16 segments a quarter turn
//     for (const geometry::Ring &ring : geometry::outlines(grown)) ...
//
// A Geometry owns its GEOS object and releases it when it goes out of scope. It refers to the Context that
// made it, which must outlive it. Geometries are moved, not copied; clone() makes a copy.
//
// Order of results. GEOS decides the order of the polygons in a result and of the holes in a polygon, and
// the vertex each ring starts at; this layer keeps that order. polygons() and outlines() add the rules of
// studio/operations.py and say so below.
//
// Errors. A GEOS failure (the reference would raise shapely.errors.GEOSException) throws geometry::Error
// with GEOS's message. In a build without GEOS (KERFKITE_WITH_GEOS=OFF) available() is false and creating
// a Context throws geometry::Error.
//
// For printed-circuit isolation, which is not written yet, the pieces are: polygon() for regions,
// point() and line() with buffer() for flashes and stroked tracks (Cap::Round for a round aperture,
// Cap::Square or Cap::Flat for a rectangular one), Artwork for dark and clear polarity, and
// buffer() followed by outlines() for the contour at each tool radius.
#ifndef KERFKITE_GEOMETRY_HPP
#define KERFKITE_GEOMETRY_HPP

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace kerfkite {
namespace geometry {

using Point = std::array<double, 2>;
// A sequence of vertices: a closed ring repeats its first vertex at the end; a line does not.
using Ring = std::vector<Point>;

// Whether this build contains GEOS.
bool available();
// The GEOS release in this build, for example "3.14.1", or an empty string without GEOS. The reference
// must use the same release for results to match: see shapely.geos_version_string.
std::string version();

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// GEOS geometry type numbers.
enum class Kind { Point = 0, LineString = 1, LinearRing = 2, Polygon = 3, MultiPoint = 4, MultiLineString = 5, MultiPolygon = 6, Collection = 7 };
// Shapely's BufferCapStyle and BufferJoinStyle, which are GEOS's numbers.
enum class Cap { Round = 1, Flat = 2, Square = 3 };
enum class Join { Round = 1, Mitre = 2, Bevel = 3 };

// The keyword arguments of Shapely's buffer(), with Shapely's defaults.
struct BufferStyle {
    int quad_segs = 16;  // segments in a quarter circle
    Cap cap = Cap::Round;
    Join join = Join::Round;
    double mitre_limit = 5.0;
    bool single_sided = false;
};

class Context;

class Geometry {
  public:
    Geometry() = default;
    Geometry(Geometry &&other) noexcept;
    Geometry &operator=(Geometry &&other) noexcept;
    Geometry(const Geometry &) = delete;
    Geometry &operator=(const Geometry &) = delete;
    ~Geometry();

    // False for a default-constructed or moved-from object, which holds nothing.
    explicit operator bool() const { return handle_ != nullptr; }

    Kind kind() const;     // geom.geom_type; GEOSGeomTypeId_r
    bool empty() const;    // geom.is_empty; GEOSisEmpty_r
    bool valid() const;    // geom.is_valid; GEOSisValid_r
    double area() const;   // geom.area; GEOSArea_r
    double length() const; // geom.length; GEOSLength_r
    // geom.bounds: least x, least y, greatest x, greatest y (GEOSGeom_getExtent_r). Not for an empty geometry.
    std::array<double, 4> bounds() const;
    Geometry clone() const;

    // Members of a multi-part geometry or collection, in GEOS's order: len(geom.geoms) and geom.geoms[i]
    // (GEOSGetNumGeometries_r, GEOSGetGeometryN_r). A single geometry has one part, itself.
    size_t parts() const;
    Geometry part(size_t index) const;

    // A polygon's rings as GEOS holds them, closed: polygon.exterior.coords and polygon.interiors[i].coords
    // (GEOSGetExteriorRing_r, GEOSGetNumInteriorRings_r, GEOSGetInteriorRingN_r).
    Ring exterior() const;
    size_t holes() const;
    Ring hole(size_t index) const;

    // The vertices of a point, line or ring: geom.coords (GEOSGeom_getCoordSeq_r).
    Ring coordinates() const;

  private:
    friend class Context;
    Geometry(Context *context, void *handle) : context_(context), handle_(handle) {}
    Context *context_ = nullptr;
    void *handle_ = nullptr;  // GEOSGeometry *
};

class Context {
  public:
    Context();
    ~Context();
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    // shapely.from_wkt(text): GEOSWKTReader_read_r.
    Geometry read_wkt(const std::string &text);
    // GeometryCollection(): the empty collection, read from text as Shapely does.
    Geometry empty_collection();
    // Point(x, y): GEOSGeom_createPointFromXY_r.
    Geometry point(double x, double y);
    // LineString(points): GEOSGeom_createLineString_r. At least two points.
    Geometry line(const Ring &points);
    // Polygon(shell, holes): GEOSGeom_createLinearRing_r and GEOSGeom_createPolygon_r. As in Shapely, a
    // ring whose last vertex is not exactly its first, or which has three vertices, is closed by repeating
    // the first. A ring needs at least four vertices once closed. Vertices are not reordered.
    Geometry polygon(const Ring &shell, const std::vector<Ring> &holes = {});
    // GeometryCollection(parts): GEOSGeom_createCollection_r. Takes the parts.
    Geometry collection(std::vector<Geometry> parts);

    // geom.buffer(distance, ...): GEOSBufferWithParams_r. A negative distance shrinks a polygon.
    Geometry buffer(const Geometry &geom, double distance, const BufferStyle &style = BufferStyle());
    // a.union(b), a.difference(b), a.intersection(b), a.symmetric_difference(b):
    // GEOSUnion_r, GEOSDifference_r, GEOSIntersection_r, GEOSSymDifference_r.
    Geometry unite(const Geometry &a, const Geometry &b);
    Geometry difference(const Geometry &a, const Geometry &b);
    Geometry intersection(const Geometry &a, const Geometry &b);
    Geometry symmetric_difference(const Geometry &a, const Geometry &b);
    // shapely.affinity.affine_transform(geom, [a, b, d, e, xoff, yoff]) for planar geometry: a copy with
    // each vertex moved to (a*x + b*y + xoff, d*x + e*y + yoff), computed in that order with each product
    // and sum rounded on its own, as NumPy computes them. The copy has the structure of the original:
    // the same parts, rings and vertex order.
    Geometry affine(const Geometry &geom, double a, double b, double d, double e, double xoff, double yoff);
    // shapely.unary_union(geom): GEOSUnaryUnion_r. The union of every member of a collection at once,
    // which is much faster than uniting them in pairs.
    Geometry unite_all(const Geometry &geom);

    void *handle() const { return handle_; }  // GEOSContextHandle_t
    // Called by GEOS with the text of a failure.
    void remember(const char *message) { error_ = message ? message : ""; }
    // Throws Error with the remembered text.
    [[noreturn]] void fail(const char *what) const;

  private:
    friend class Geometry;
    Geometry own(void *handle, const char *what);
    void *sequence(const Ring &points, bool ring);
    void *moved(const void *geom, const double *matrix);
    void *handle_ = nullptr;
    std::string error_;
};

// studio.operations.polygons(g): a polygon gives itself; a multi-part geometry or collection gives its
// members that are polygons, in GEOS's order; anything else gives nothing.
std::vector<Geometry> polygons(const Geometry &geom);

// studio.operations.outlines(g): the rings of every polygon, smallest polygon first (by area; polygons of
// equal area keep GEOS's order). For each polygon its holes in GEOS's order, then its outer ring. Each
// ring is closed and starts at the vertex GEOS gives it.
std::vector<Ring> outlines(const Geometry &geom);

// Dark and clear artwork, as a photoplotter file draws it: a dark shape adds to the image and a clear
// shape removes from what has been drawn so far. Shapes are taken in drawing order. Consecutive dark
// shapes are united in one step.
class Artwork {
  public:
    explicit Artwork(Context &context) : context_(context) {}
    void dark(Geometry shape);
    void clear(Geometry shape);
    // The image so far: empty, a polygon or a multi-polygon. The artwork may be added to afterwards.
    Geometry image();

  private:
    void settle();
    Context &context_;
    Geometry image_;
    std::vector<Geometry> pending_;
};

}  // namespace geometry
}  // namespace kerfkite

#endif
