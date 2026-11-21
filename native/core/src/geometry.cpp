// Planar geometry over the GEOS C API: see geometry.hpp. Every GEOS call goes through a context (the _r
// functions), so nothing here touches global state.
#include "geometry.hpp"

#include <algorithm>
#include <utility>

#ifdef KERFKITE_WITH_GEOS

#include <geos_c.h>

namespace kerfkite {
namespace geometry {

namespace {

GEOSContextHandle_t raw(const Context *context) { return static_cast<GEOSContextHandle_t>(context->handle()); }
GEOSGeometry *raw(void *handle) { return static_cast<GEOSGeometry *>(handle); }

void on_error(const char *message, void *context) { static_cast<Context *>(context)->remember(message); }
// Notices are remarks, such as the reason a polygon is invalid; the reference ignores them too.
void on_notice(const char *, void *) {}

Ring read(const Context *context, const GEOSGeometry *line) {
    const GEOSCoordSequence *sequence = GEOSGeom_getCoordSeq_r(raw(context), line);
    unsigned int size = 0;
    if (!sequence || !GEOSCoordSeq_getSize_r(raw(context), sequence, &size)) context->fail("coordinates");
    Ring ring(size);
    // Rows of x, y as doubles: the values GEOS holds, with nothing rounded.
    if (size && !GEOSCoordSeq_copyToBuffer_r(raw(context), sequence, &ring[0][0], 0, 0)) context->fail("coordinates");
    return ring;
}

}  // namespace

bool available() { return true; }
std::string version() { return std::to_string(GEOS_VERSION_MAJOR) + "." + std::to_string(GEOS_VERSION_MINOR) + "." + std::to_string(GEOS_VERSION_PATCH); }

Context::Context() {
    handle_ = GEOS_init_r();
    if (!handle_) throw Error("GEOS could not be started");
    GEOSContext_setErrorMessageHandler_r(raw(this), on_error, this);
    GEOSContext_setNoticeMessageHandler_r(raw(this), on_notice, this);
}

Context::~Context() {
    if (handle_) GEOS_finish_r(raw(this));
}

void Context::fail(const char *what) const {
    throw Error(std::string("GEOS ") + what + (error_.empty() ? std::string(" failed") : ": " + error_));
}

Geometry Context::own(void *handle, const char *what) {
    if (!handle) fail(what);
    return Geometry(this, handle);
}

// shapely's coordseq_from_buffer: the values as given, and for a ring one more vertex equal to the first
// when there are three vertices or the last differs from the first in either ordinate.
void *Context::sequence(const Ring &points, bool ring) {
    const size_t size = points.size();
    const bool closure = ring && size > 0 && (size == 3 || points[0][0] != points[size - 1][0] || points[0][1] != points[size - 1][1]);
    GEOSCoordSequence *sequence = GEOSCoordSeq_create_r(raw(this), static_cast<unsigned int>(size + (closure ? 1 : 0)), 2);
    if (!sequence) fail("coordinate sequence");
    for (size_t i = 0; i < size + (closure ? 1 : 0); ++i) {
        const Point &point = points[i < size ? i : 0];
        if (!GEOSCoordSeq_setXY_r(raw(this), sequence, static_cast<unsigned int>(i), point[0], point[1])) {
            GEOSCoordSeq_destroy_r(raw(this), sequence);
            fail("coordinate sequence");
        }
    }
    return sequence;
}

Geometry Context::read_wkt(const std::string &text) {
    error_.clear();
    GEOSWKTReader *reader = GEOSWKTReader_create_r(raw(this));
    if (!reader) fail("text reader");
    GEOSGeometry *geom = GEOSWKTReader_read_r(raw(this), reader, text.c_str());
    GEOSWKTReader_destroy_r(raw(this), reader);
    return own(geom, "text reader");
}

Geometry Context::empty_collection() { return read_wkt("GEOMETRYCOLLECTION EMPTY"); }

Geometry Context::point(double x, double y) {
    error_.clear();
    return own(GEOSGeom_createPointFromXY_r(raw(this), x, y), "point");
}

Geometry Context::line(const Ring &points) {
    error_.clear();
    if (points.size() < 2) throw Error("A line needs at least two points");
    return own(GEOSGeom_createLineString_r(raw(this), static_cast<GEOSCoordSequence *>(sequence(points, false))), "line");
}

Geometry Context::polygon(const Ring &shell, const std::vector<Ring> &holes) {
    error_.clear();
    const auto ring = [this](const Ring &points) {
        // shapely.linearrings refuses a ring that still has fewer than four vertices once closed.
        if (points.size() < 3) throw Error("A linearring requires at least 4 coordinates.");
        return own(GEOSGeom_createLinearRing_r(raw(this), static_cast<GEOSCoordSequence *>(sequence(points, true))), "ring");
    };
    Geometry outer = ring(shell);
    std::vector<Geometry> inner;
    inner.reserve(holes.size());
    for (const Ring &hole : holes) inner.push_back(ring(hole));
    std::vector<GEOSGeometry *> handles;
    for (Geometry &hole : inner) handles.push_back(raw(std::exchange(hole.handle_, nullptr)));
    // GEOS takes the rings whether or not it succeeds.
    return own(GEOSGeom_createPolygon_r(raw(this), raw(std::exchange(outer.handle_, nullptr)), handles.empty() ? nullptr : handles.data(),
                                        static_cast<unsigned int>(handles.size())),
               "polygon");
}

Geometry Context::collection(std::vector<Geometry> parts) {
    error_.clear();
    std::vector<GEOSGeometry *> handles;
    handles.reserve(parts.size());
    for (Geometry &part : parts) {
        if (!part || part.context_ != this) throw Error("A collection needs geometries of this context");
        handles.push_back(raw(std::exchange(part.handle_, nullptr)));
    }
    return own(GEOSGeom_createCollection_r(raw(this), GEOS_GEOMETRYCOLLECTION, handles.empty() ? nullptr : handles.data(),
                                           static_cast<unsigned int>(handles.size())),
               "collection");
}

// shapely's buffer ufunc: parameters object, the five settings in this order, GEOSBufferWithParams_r.
Geometry Context::buffer(const Geometry &geom, double distance, const BufferStyle &style) {
    error_.clear();
    GEOSBufferParams *params = GEOSBufferParams_create_r(raw(this));
    if (!params) fail("buffer");
    const bool set = GEOSBufferParams_setQuadrantSegments_r(raw(this), params, style.quad_segs) &&
                     GEOSBufferParams_setEndCapStyle_r(raw(this), params, static_cast<int>(style.cap)) &&
                     GEOSBufferParams_setJoinStyle_r(raw(this), params, static_cast<int>(style.join)) &&
                     GEOSBufferParams_setMitreLimit_r(raw(this), params, style.mitre_limit) &&
                     GEOSBufferParams_setSingleSided_r(raw(this), params, style.single_sided ? 1 : 0);
    GEOSGeometry *result = set ? GEOSBufferWithParams_r(raw(this), raw(geom.handle_), params, distance) : nullptr;
    GEOSBufferParams_destroy_r(raw(this), params);
    return own(result, "buffer");
}

Geometry Context::unite(const Geometry &a, const Geometry &b) {
    error_.clear();
    return own(GEOSUnion_r(raw(this), raw(a.handle_), raw(b.handle_)), "union");
}

Geometry Context::difference(const Geometry &a, const Geometry &b) {
    error_.clear();
    return own(GEOSDifference_r(raw(this), raw(a.handle_), raw(b.handle_)), "difference");
}

Geometry Context::intersection(const Geometry &a, const Geometry &b) {
    error_.clear();
    return own(GEOSIntersection_r(raw(this), raw(a.handle_), raw(b.handle_)), "intersection");
}

Geometry Context::symmetric_difference(const Geometry &a, const Geometry &b) {
    error_.clear();
    return own(GEOSSymDifference_r(raw(this), raw(a.handle_), raw(b.handle_)), "symmetric difference");
}

// shapely's set_coordinates: the same structure rebuilt around new vertices. Returns null on failure.
void *Context::moved(const void *source, const double *m) {
    const GEOSGeometry *geom = static_cast<const GEOSGeometry *>(source);
    const int type = GEOSGeomTypeId_r(raw(this), geom);
    if (type >= 0 && type <= 2) {
        if (type == 0 && GEOSisEmpty_r(raw(this), geom) == 1) return GEOSGeom_createEmptyPoint_r(raw(this));
        const GEOSCoordSequence *old = GEOSGeom_getCoordSeq_r(raw(this), geom);
        unsigned int size = 0;
        if (!old || !GEOSCoordSeq_getSize_r(raw(this), old, &size)) return nullptr;
        GEOSCoordSequence *fresh = GEOSCoordSeq_create_r(raw(this), size, 2);
        if (!fresh) return nullptr;
        for (unsigned int i = 0; i < size; ++i) {
            double x = 0, y = 0;
            if (!GEOSCoordSeq_getXY_r(raw(this), old, i, &x, &y)) {
                GEOSCoordSeq_destroy_r(raw(this), fresh);
                return nullptr;
            }
            // Two products, their sum, then the offset: four rounded operations for each ordinate. The
            // core is compiled without contraction, so no product is fused into a sum.
            const double ax = m[0] * x, by = m[1] * y, dx = m[2] * x, ey = m[3] * y;
            const double xp = ax + by + m[4], yp = dx + ey + m[5];
            if (!GEOSCoordSeq_setXY_r(raw(this), fresh, i, xp, yp)) {
                GEOSCoordSeq_destroy_r(raw(this), fresh);
                return nullptr;
            }
        }
        if (type == 0) return GEOSGeom_createPoint_r(raw(this), fresh);
        return type == 1 ? GEOSGeom_createLineString_r(raw(this), fresh) : GEOSGeom_createLinearRing_r(raw(this), fresh);
    }
    const auto release = [this](std::vector<GEOSGeometry *> &made) {
        for (GEOSGeometry *one : made) GEOSGeom_destroy_r(raw(this), one);
    };
    if (type == 3) {
        const int holes = GEOSGetNumInteriorRings_r(raw(this), geom);
        const GEOSGeometry *shell = GEOSGetExteriorRing_r(raw(this), geom);
        if (holes < 0 || !shell) return nullptr;
        GEOSGeometry *outer = raw(moved(shell, m));
        if (!outer) return nullptr;
        std::vector<GEOSGeometry *> inner;
        for (int i = 0; i < holes; ++i) {
            const GEOSGeometry *hole = GEOSGetInteriorRingN_r(raw(this), geom, i);
            GEOSGeometry *copy = hole ? raw(moved(hole, m)) : nullptr;
            if (!copy) {
                GEOSGeom_destroy_r(raw(this), outer);
                release(inner);
                return nullptr;
            }
            inner.push_back(copy);
        }
        return GEOSGeom_createPolygon_r(raw(this), outer, inner.empty() ? nullptr : inner.data(), static_cast<unsigned int>(inner.size()));
    }
    if (type >= 4 && type <= 7) {
        const int count = GEOSGetNumGeometries_r(raw(this), geom);
        if (count < 0) return nullptr;
        std::vector<GEOSGeometry *> members;
        for (int i = 0; i < count; ++i) {
            const GEOSGeometry *member = GEOSGetGeometryN_r(raw(this), geom, i);
            GEOSGeometry *copy = member ? raw(moved(member, m)) : nullptr;
            if (!copy) {
                release(members);
                return nullptr;
            }
            members.push_back(copy);
        }
        return GEOSGeom_createCollection_r(raw(this), type, members.empty() ? nullptr : members.data(), static_cast<unsigned int>(members.size()));
    }
    return nullptr;
}

Geometry Context::affine(const Geometry &geom, double a, double b, double d, double e, double xoff, double yoff) {
    error_.clear();
    const double matrix[6] = {a, b, d, e, xoff, yoff};
    return own(moved(geom.handle_, matrix), "transform");
}

Geometry Context::unite_all(const Geometry &geom) {
    error_.clear();
    return own(GEOSUnaryUnion_r(raw(this), raw(geom.handle_)), "union");
}

Geometry::Geometry(Geometry &&other) noexcept : context_(other.context_), handle_(std::exchange(other.handle_, nullptr)) {}

Geometry &Geometry::operator=(Geometry &&other) noexcept {
    if (this != &other) {
        if (handle_) GEOSGeom_destroy_r(raw(context_), raw(handle_));
        context_ = other.context_;
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Geometry::~Geometry() {
    if (handle_) GEOSGeom_destroy_r(raw(context_), raw(handle_));
}

Kind Geometry::kind() const {
    const int type = GEOSGeomTypeId_r(raw(context_), raw(handle_));
    if (type < 0 || type > 7) context_->fail("geometry type");
    return static_cast<Kind>(type);
}

bool Geometry::empty() const {
    const char answer = GEOSisEmpty_r(raw(context_), raw(handle_));
    if (answer == 2) context_->fail("emptiness test");
    return answer == 1;
}

bool Geometry::valid() const {
    const char answer = GEOSisValid_r(raw(context_), raw(handle_));
    if (answer == 2) context_->fail("validity test");
    return answer == 1;
}

double Geometry::area() const {
    double value = 0;
    if (!GEOSArea_r(raw(context_), raw(handle_), &value)) context_->fail("area");
    return value;
}

double Geometry::length() const {
    double value = 0;
    if (!GEOSLength_r(raw(context_), raw(handle_), &value)) context_->fail("length");
    return value;
}

std::array<double, 4> Geometry::bounds() const {
    std::array<double, 4> box{};
    if (empty() || !GEOSGeom_getExtent_r(raw(context_), raw(handle_), &box[0], &box[1], &box[2], &box[3])) context_->fail("bounds");
    return box;
}

Geometry Geometry::clone() const { return context_->own(GEOSGeom_clone_r(raw(context_), raw(handle_)), "copy"); }

size_t Geometry::parts() const {
    const int count = GEOSGetNumGeometries_r(raw(context_), raw(handle_));
    if (count < 0) context_->fail("member count");
    return static_cast<size_t>(count);
}

Geometry Geometry::part(size_t index) const {
    const GEOSGeometry *member = GEOSGetGeometryN_r(raw(context_), raw(handle_), static_cast<int>(index));
    if (!member) context_->fail("member");
    return context_->own(GEOSGeom_clone_r(raw(context_), member), "copy");
}

Ring Geometry::exterior() const {
    const GEOSGeometry *ring = GEOSGetExteriorRing_r(raw(context_), raw(handle_));
    if (!ring) context_->fail("outer ring");
    return read(context_, ring);
}

size_t Geometry::holes() const {
    const int count = GEOSGetNumInteriorRings_r(raw(context_), raw(handle_));
    if (count < 0) context_->fail("hole count");
    return static_cast<size_t>(count);
}

Ring Geometry::hole(size_t index) const {
    const GEOSGeometry *ring = GEOSGetInteriorRingN_r(raw(context_), raw(handle_), static_cast<int>(index));
    if (!ring) context_->fail("hole");
    return read(context_, ring);
}

Ring Geometry::coordinates() const { return read(context_, raw(handle_)); }

std::vector<Geometry> polygons(const Geometry &geom) {
    std::vector<Geometry> result;
    const Kind kind = geom.kind();
    if (kind == Kind::Polygon) {
        result.push_back(geom.clone());
    } else if (kind == Kind::MultiPoint || kind == Kind::MultiLineString || kind == Kind::MultiPolygon || kind == Kind::Collection) {
        const size_t count = geom.parts();
        for (size_t i = 0; i < count; ++i) {
            Geometry member = geom.part(i);
            if (member.kind() == Kind::Polygon) result.push_back(std::move(member));
        }
    }
    return result;
}

std::vector<Ring> outlines(const Geometry &geom) {
    std::vector<Geometry> parts = polygons(geom);
    // sorted(..., key=area): ascending, and stable, so equal areas keep their order.
    std::vector<std::pair<double, size_t>> order;
    order.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) order.emplace_back(parts[i].area(), i);
    std::stable_sort(order.begin(), order.end(), [](const std::pair<double, size_t> &a, const std::pair<double, size_t> &b) { return a.first < b.first; });
    std::vector<Ring> result;
    for (const std::pair<double, size_t> &entry : order) {
        const Geometry &polygon = parts[entry.second];
        const size_t holes = polygon.holes();
        for (size_t i = 0; i < holes; ++i) result.push_back(polygon.hole(i));
        result.push_back(polygon.exterior());
    }
    return result;
}

void Artwork::dark(Geometry shape) { pending_.push_back(std::move(shape)); }

void Artwork::clear(Geometry shape) {
    settle();
    if (image_) image_ = context_.difference(image_, shape);
}

Geometry Artwork::image() {
    settle();
    return image_ ? image_.clone() : context_.read_wkt("POLYGON EMPTY");
}

void Artwork::settle() {
    if (pending_.empty()) return;
    if (image_) pending_.insert(pending_.begin(), std::move(image_));
    image_ = context_.unite_all(context_.collection(std::move(pending_)));
    pending_.clear();
}

}  // namespace geometry
}  // namespace kerfkite

#else  // a build without GEOS: the layer is present and refuses to start

namespace kerfkite {
namespace geometry {

namespace {
[[noreturn]] void absent() { throw Error("This build has no GEOS"); }
}  // namespace

bool available() { return false; }
std::string version() { return std::string(); }

Context::Context() { absent(); }
Context::~Context() {}
void Context::fail(const char *) const { absent(); }
Geometry Context::own(void *, const char *) { absent(); }
void *Context::sequence(const Ring &, bool) { absent(); }
Geometry Context::read_wkt(const std::string &) { absent(); }
Geometry Context::empty_collection() { absent(); }
Geometry Context::point(double, double) { absent(); }
Geometry Context::line(const Ring &) { absent(); }
Geometry Context::polygon(const Ring &, const std::vector<Ring> &) { absent(); }
Geometry Context::collection(std::vector<Geometry>) { absent(); }
Geometry Context::buffer(const Geometry &, double, const BufferStyle &) { absent(); }
Geometry Context::unite(const Geometry &, const Geometry &) { absent(); }
Geometry Context::difference(const Geometry &, const Geometry &) { absent(); }
Geometry Context::intersection(const Geometry &, const Geometry &) { absent(); }
Geometry Context::symmetric_difference(const Geometry &, const Geometry &) { absent(); }
Geometry Context::unite_all(const Geometry &) { absent(); }
Geometry Context::affine(const Geometry &, double, double, double, double, double, double) { absent(); }
void *Context::moved(const void *, const double *) { absent(); }

Geometry::Geometry(Geometry &&other) noexcept : context_(other.context_), handle_(std::exchange(other.handle_, nullptr)) {}
Geometry &Geometry::operator=(Geometry &&other) noexcept {
    context_ = other.context_;
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}
Geometry::~Geometry() {}
Kind Geometry::kind() const { absent(); }
bool Geometry::empty() const { absent(); }
bool Geometry::valid() const { absent(); }
double Geometry::area() const { absent(); }
double Geometry::length() const { absent(); }
std::array<double, 4> Geometry::bounds() const { absent(); }
Geometry Geometry::clone() const { absent(); }
size_t Geometry::parts() const { absent(); }
Geometry Geometry::part(size_t) const { absent(); }
Ring Geometry::exterior() const { absent(); }
size_t Geometry::holes() const { absent(); }
Ring Geometry::hole(size_t) const { absent(); }
Ring Geometry::coordinates() const { absent(); }

std::vector<Geometry> polygons(const Geometry &) { absent(); }
std::vector<Ring> outlines(const Geometry &) { absent(); }
void Artwork::dark(Geometry) { absent(); }
void Artwork::clear(Geometry) { absent(); }
Geometry Artwork::image() { absent(); }
void Artwork::settle() { absent(); }

}  // namespace geometry
}  // namespace kerfkite

#endif
