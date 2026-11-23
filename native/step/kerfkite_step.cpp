// kerfkite-step: STEP exchange for KerfKite Studio through Open CASCADE Technology.
//
// Open CASCADE is LGPL 2.1 (with the OCCT exception), so it is kept apart from the MIT-licensed application: this
// program is the only part that uses it, it links Open CASCADE's shared libraries (which can be replaced with
// another build of the same version), and the workstation server starts it as a separate process for each STEP
// request. Without it, the server answers STEP requests with the message that STEP needs Open CASCADE.
//
// One JSON request on standard input, one JSON answer on standard output:
//   {"inspect": "<base64 STEP>"} -> the shape tessellated as studio/cad_exchange.py's _mesh_shape does it:
//       {"vertices", "triangles", "bounds", "area", "faces", "solids", "solidOnly", "volume"}
//   {"export": {"faces": [...]}}     -> {"step": "<base64>"}: a surface, as the core's "step-faces" gives its faces
//   {"export": {"features": [...]}}  -> {"step": "<base64>"}: a part, as the core's "step-part" gives its steps
//   {"export": {"source": "<base64 STEP>"}} -> {"step": "<base64>"}: an imported document written again
//   {"version": true} -> {"open_cascade": "7.9.3"}
// A failure answers {"error": "<message>"}, with the reference's messages. The server checks the STEP text first
// (its size, header, end and entity count); this program reads only what the server has checked.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <exception>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_BezierSurface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopAbs.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "base64.hpp"
#include "kerfkite/json.hpp"

namespace {

using kerfkite::Json;

// A failure with the reference's message, answered as {"error": ...}.
struct Refusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const size_t kMaxTriangles = 60000;
const double kDegree = 3.141592653589793 / 180.0;  // math.radians: degrees times pi / 180

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

std::string decoded(const Json &text) {
    std::vector<unsigned char> bytes;
    if (!text.is_string() || !kerfkite::detail::base64(text.get<std::string>(), bytes)) throw Refusal("Invalid STEP file encoding");
    return std::string(bytes.begin(), bytes.end());
}

double number(const Json &value) { return value.get<double>(); }

gp_Pnt point(const Json &p) { return gp_Pnt(number(p.at(0)), number(p.at(1)), number(p.at(2))); }

gp_Trsf turn(const Json &axis, double degrees, const Json &center) {
    gp_Trsf t;
    t.SetRotation(gp_Ax1(point(center), gp_Dir(number(axis.at(0)), number(axis.at(1)), number(axis.at(2)))), degrees * kDegree);
    return t;
}

gp_Trsf move(const Json &by) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(number(by.at(0)), number(by.at(1)), number(by.at(2))));
    return t;
}

TopoDS_Shape transformed(const TopoDS_Shape &shape, const gp_Trsf &t) { return BRepBuilderAPI_Transform(shape, t).Shape(); }

// ---- reading: cad_exchange._read_shape and _mesh_shape --------------------------------------------------

TopoDS_Shape read_shape(const std::string &raw) {
    STEPControl_Reader reader;
    std::istringstream stream(raw, std::ios::in | std::ios::binary);
    if (reader.ReadStream("studio.step", stream) != IFSelect_RetDone) throw Refusal("Open CASCADE could not read this STEP document");
    reader.SetSystemLengthUnit(1.0);  // translated geometry in millimetres
    const int roots = reader.NbRootsForTransfer();
    if (roots < 1 || roots > 256 || reader.TransferRoots() == 0) throw Refusal("STEP contains no transferable geometry or too many roots");
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) throw Refusal("STEP contains no transferable shape");
    return shape;
}

std::vector<TopoDS_Shape> components(const TopoDS_Shape &shape, TopAbs_ShapeEnum kind) {
    std::vector<TopoDS_Shape> out;
    for (TopExp_Explorer it(shape, kind); it.More(); it.Next()) out.push_back(it.Current());
    return out;
}

Json mesh_shape(const TopoDS_Shape &shape) {
    const std::vector<TopoDS_Shape> faces = components(shape, TopAbs_FACE);
    if (faces.empty() || faces.size() > 2000) throw Refusal("Import a shape with 1\xe2\x80\x93" "2000 faces");
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(shape, bounds, Standard_False, Standard_False);
    double box[6];
    bounds.Get(box[0], box[1], box[2], box[3], box[4], box[5]);
    for (const double b : box)
        if (!std::isfinite(b) || std::fabs(b) > 100000) throw Refusal("STEP geometry exceeds the 100000 mm modelling range");
    // np.linalg.norm of the diagonal, as NumPy's dot product (OpenBLAS) sums the squares: with fused multiply-adds.
    const double dx = box[3] - box[0], dy = box[4] - box[1], dz = box[5] - box[2];
    const double diagonal = std::sqrt(std::fma(dz, dz, std::fma(dy, dy, dx * dx)));
    BRepMesh_IncrementalMesh mesher(shape, std::max(0.02, diagonal / 1500), Standard_False, 0.18, Standard_False);
    if (!mesher.IsDone()) throw Refusal("Could not tessellate this STEP shape");
    Json vertices = Json::array(), triangles = Json::array();
    size_t count = 0, triangle_count = 0;
    for (const TopoDS_Shape &sub : faces) {
        const TopoDS_Face face = TopoDS::Face(sub);
        TopLoc_Location location;
        const Handle(Poly_Triangulation) &mesh = BRep_Tool::Triangulation(face, location);
        if (mesh.IsNull() || mesh->NbTriangles() == 0) throw Refusal("STEP contains an unmeshable face");
        if (static_cast<size_t>(mesh->NbTriangles()) + triangle_count > kMaxTriangles) throw Refusal("STEP preview exceeds 60000 triangles");
        const long long offset = static_cast<long long>(count) - 1;  // OCCT's nodes start at one
        const gp_Trsf trsf = location.Transformation();
        for (int i = 1; i <= mesh->NbNodes(); ++i) {
            const gp_Pnt p = mesh->Node(i).Transformed(trsf);
            vertices.push_back(Json::array({p.X(), p.Y(), p.Z()}));
            ++count;
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= mesh->NbTriangles(); ++i) {
            int a = 0, b = 0, c = 0;
            mesh->Triangle(i).Get(a, b, c);
            if (reversed) std::swap(b, c);
            triangles.push_back(Json::array({offset + a, offset + b, offset + c}));
            ++triangle_count;
        }
    }
    const std::vector<TopoDS_Shape> solids = components(shape, TopAbs_SOLID);
    size_t solid_faces = 0;
    for (const TopoDS_Shape &solid : solids) solid_faces += components(solid, TopAbs_FACE).size();
    GProp_GProps surface;
    BRepGProp::SurfaceProperties(shape, surface);
    GProp_GProps volume;
    BRepGProp::VolumeProperties(shape, volume, Standard_True);
    Json out = Json::object();
    out["vertices"] = std::move(vertices);
    out["triangles"] = std::move(triangles);
    out["bounds"] = Json::array({box[0], box[1], box[2], box[3], box[4], box[5]});
    out["area"] = surface.Mass();
    out["faces"] = static_cast<long long>(faces.size());
    out["solids"] = static_cast<long long>(solids.size());
    out["solidOnly"] = !solids.empty() && solid_faces == faces.size();
    out["volume"] = volume.Mass();
    return out;
}

// ---- surfaces: cad_exchange._surface_face and _surface_shape --------------------------------------------

Handle(Geom_Surface) patch_surface(const Json &face) {
    const Json &poles = face.at("poles"), &weights = face.at("weights");
    const int rows = static_cast<int>(poles.size()), columns = static_cast<int>(poles.at(0).size());
    TColgp_Array2OfPnt grid(1, rows, 1, columns);
    TColStd_Array2OfReal weight_grid(1, rows, 1, columns);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < columns; ++j) {
            grid.SetValue(i + 1, j + 1, point(poles.at(static_cast<size_t>(i)).at(static_cast<size_t>(j))));
            weight_grid.SetValue(i + 1, j + 1, number(weights.at(static_cast<size_t>(i)).at(static_cast<size_t>(j))));
        }
    if (face.at("kind").get<std::string>() == "bezier") return new Geom_BezierSurface(grid, weight_grid);
    const Json &knots = face.at("knots"), &multiplicities = face.at("multiplicities"), &degrees = face.at("degrees");
    TColStd_Array1OfReal u_knots(1, static_cast<int>(knots.at(0).size())), v_knots(1, static_cast<int>(knots.at(1).size()));
    TColStd_Array1OfInteger u_mults(1, static_cast<int>(knots.at(0).size())), v_mults(1, static_cast<int>(knots.at(1).size()));
    for (size_t k = 0; k < knots.at(0).size(); ++k) {
        u_knots.SetValue(static_cast<int>(k) + 1, number(knots.at(0).at(k)));
        u_mults.SetValue(static_cast<int>(k) + 1, multiplicities.at(0).at(k).get<int>());
    }
    for (size_t k = 0; k < knots.at(1).size(); ++k) {
        v_knots.SetValue(static_cast<int>(k) + 1, number(knots.at(1).at(k)));
        v_mults.SetValue(static_cast<int>(k) + 1, multiplicities.at(1).at(k).get<int>());
    }
    return new Geom_BSplineSurface(grid, weight_grid, u_knots, v_knots, u_mults, v_mults, degrees.at(0).get<int>(), degrees.at(1).get<int>());
}

TopoDS_Wire uv_wire(const Json &ring, const Handle(Geom_Surface) &surface) {
    BRepBuilderAPI_MakeWire wire;
    for (size_t k = 0; k + 1 < ring.size(); ++k) {
        const Json &a = ring.at(k), &b = ring.at(k + 1);
        const Handle(Geom2d_TrimmedCurve) segment =
            GCE2d_MakeSegment(gp_Pnt2d(number(a.at(0)), number(a.at(1))), gp_Pnt2d(number(b.at(0)), number(b.at(1)))).Value();
        wire.Add(BRepBuilderAPI_MakeEdge(segment, surface).Edge());
        if (!wire.IsDone()) throw Refusal("Could not connect a UV trim wire");
    }
    return wire.Wire();
}

TopoDS_Face surface_face(const Json &face) {
    const Handle(Geom_Surface) surface = patch_surface(face);
    if (face.contains("domain")) {
        const Json &domain = face.at("domain");
        BRepBuilderAPI_MakeFace maker(surface, uv_wire(domain.at(0), surface), Standard_True);
        for (size_t h = 1; h < domain.size(); ++h) maker.Add(uv_wire(domain.at(h), surface));
        if (!maker.IsDone()) throw Refusal("Could not trim this STEP face");
        TopoDS_Face trimmed = maker.Face();
        if (!BRepLib::BuildCurves3d(trimmed) || !BRepCheck_Analyzer(trimmed).IsValid())
            throw Refusal("The UV outline could not form a valid STEP face");
        return trimmed;
    }
    const Json &w = face.at("window");
    BRepBuilderAPI_MakeFace maker(surface, number(w.at(0)), number(w.at(1)), number(w.at(2)), number(w.at(3)), 1e-7);
    if (!maker.IsDone()) throw Refusal("Could not build the spline face");
    return maker.Face();
}

TopoDS_Shape surface_shape(const Json &faces) {
    std::vector<TopoDS_Face> built;
    for (const Json &face : faces) {
        if (face.contains("error")) throw Refusal(face.at("error").get<std::string>());
        built.push_back(surface_face(face));
    }
    if (built.size() == 1) return built[0];
    BRepBuilderAPI_Sewing sewing(1e-7);
    for (const TopoDS_Face &face : built) sewing.Add(face);
    sewing.Perform();
    const TopoDS_Shape shape = sewing.SewedShape();
    if (shape.IsNull() || shape.ShapeType() != TopAbs_SHELL || sewing.NbDeletedFaces() != 0 || sewing.NbMultipleEdges() != 0 ||
        !BRepCheck_Analyzer(shape).IsValid())
        throw Refusal("The trimmed patches could not form a valid connected STEP shell");
    return shape;
}

// ---- parts: cad_exchange._part_shape, sketch_profile.step_profile_face and solid_loft.loft_shape -----------

TopoDS_Face profile_face(const Json &edges) {
    BRepBuilderAPI_MakeWire wire;
    for (const Json &edge : edges) {
        if (edge.contains("bezier")) {
            const Json &poles = edge.at("bezier");
            TColgp_Array1OfPnt array(1, 4);
            for (int k = 0; k < 4; ++k) array.SetValue(k + 1, point(poles.at(static_cast<size_t>(k))));
            BRepBuilderAPI_MakeEdge made(new Geom_BezierCurve(array));
            if (!made.IsDone()) throw Refusal("STEP could not construct the cubic sketch edge");
            wire.Add(made.Edge());
        } else if (edge.contains("line")) {
            const Json &ends = edge.at("line");
            wire.Add(BRepBuilderAPI_MakeEdge(point(ends.at(0)), point(ends.at(1))).Edge());
        } else {
            const Json &arc_points = edge.at("arc");
            GC_MakeArcOfCircle arc(point(arc_points.at(0)), point(arc_points.at(1)), point(arc_points.at(2)));
            if (!arc.IsDone()) throw Refusal("STEP could not form a circular sketch fillet");
            wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
        }
    }
    if (!wire.IsDone()) throw Refusal("STEP could not close the sketch wire");
    BRepBuilderAPI_MakeFace face(wire.Wire());
    if (!face.IsDone()) throw Refusal("STEP could not form a planar sketch face");
    return face.Face();
}

TopoDS_Shape loft_shape(const Json &loft) {
    const Json &sides = loft.at("sides"), &weights = loft.at("weights");
    BRepBuilderAPI_MakeWire caps[2];
    BRepBuilderAPI_Sewing sewing(1e-7);
    for (size_t side = 0; side < 4; ++side) {
        TColgp_Array2OfPnt poles(1, 3, 1, 2);
        TColStd_Array2OfReal pole_weights(1, 3, 1, 2);
        for (size_t j = 0; j < 2; ++j) {
            TColgp_Array1OfPnt row(1, 3);
            TColStd_Array1OfReal values(1, 3);
            for (size_t i = 0; i < 3; ++i) {
                const gp_Pnt p = point(sides.at(side).at(j).at(i));
                const double w = number(weights.at(i));
                poles.SetValue(static_cast<int>(i) + 1, static_cast<int>(j) + 1, p);
                pole_weights.SetValue(static_cast<int>(i) + 1, static_cast<int>(j) + 1, w);
                row.SetValue(static_cast<int>(i) + 1, p);
                values.SetValue(static_cast<int>(i) + 1, w);
            }
            caps[j].Add(BRepBuilderAPI_MakeEdge(new Geom_BezierCurve(row, values)).Edge());
        }
        sewing.Add(BRepBuilderAPI_MakeFace(new Geom_BezierSurface(poles, pole_weights), 1e-7).Face());
    }
    for (int i = 0; i < 2; ++i) {
        const TopoDS_Face cap = BRepBuilderAPI_MakeFace(caps[i].Wire(), Standard_True).Face();
        if (i == 0) sewing.Add(cap.Reversed());
        else sewing.Add(cap);
    }
    sewing.Perform();
    const TopoDS_Shape shell = sewing.SewedShape();
    if (shell.IsNull() || shell.ShapeType() != TopAbs_SHELL || sewing.NbFreeEdges() != 0)
        throw Refusal("The loft sections could not form a closed STEP shell");
    TopoDS_Solid solid = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shell)).Solid();
    if (!BRepLib::OrientClosedSolid(solid) || !BRepCheck_Analyzer(solid).IsValid())
        throw Refusal("The exact loft could not form a valid STEP solid");
    return solid;
}

TopoDS_Shape combine(const TopoDS_Shape &left, const TopoDS_Shape &right, const std::string &op) {
    TopoDS_Shape result;
    bool done = false;
    if (op == "add") {
        BRepAlgoAPI_Fuse builder(left, right);
        done = builder.IsDone();
        if (done) result = builder.Shape();
    } else if (op == "cut") {
        BRepAlgoAPI_Cut builder(left, right);
        done = builder.IsDone();
        if (done) result = builder.Shape();
    } else {
        BRepAlgoAPI_Common builder(left, right);
        done = builder.IsDone();
        if (done) result = builder.Shape();
    }
    if (!done || result.IsNull()) throw Refusal("Could not apply a STEP solid Boolean");
    return result;
}

TopoDS_Shape feature_shape(const Json &shape) {
    if (shape.contains("error")) throw Refusal(shape.at("error").get<std::string>());
    if (shape.contains("box")) {
        const Json &size = shape.at("box");
        return BRepPrimAPI_MakeBox(number(size.at(0)), number(size.at(1)), number(size.at(2))).Shape();
    }
    if (shape.contains("sphere")) return BRepPrimAPI_MakeSphere(number(shape.at("sphere"))).Shape();
    if (shape.contains("cylinder")) {
        const Json &c = shape.at("cylinder");
        return BRepPrimAPI_MakeCylinder(number(c.at(0)), number(c.at(1))).Shape();
    }
    if (shape.contains("cone")) {
        const Json &c = shape.at("cone");
        return BRepPrimAPI_MakeCone(number(c.at(0)), number(c.at(1)), number(c.at(2))).Shape();
    }
    if (shape.contains("loft")) return loft_shape(shape.at("loft"));
    if (shape.contains("prism")) {
        const Json &prism = shape.at("prism");
        return BRepPrimAPI_MakePrism(profile_face(prism.at("edges")), gp_Vec(0, 0, number(prism.at("height")))).Shape();
    }
    if (shape.contains("revol")) {
        const Json &revol = shape.at("revol");
        return BRepPrimAPI_MakeRevol(profile_face(revol.at("edges")), gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), number(revol.at("degrees")) * kDegree)
            .Shape();
    }
    if (shape.contains("step")) return read_shape(decoded(shape.at("step")));
    throw Refusal("This feature has no STEP translation");
}

TopoDS_Shape part_shape(const Json &features) {
    TopoDS_Shape result;
    bool have = false;
    for (const Json &feature : features) {
        TopoDS_Shape shape = feature_shape(feature.at("shape"));
        for (const Json &t : feature.at("turns")) shape = transformed(shape, turn(t.at("axis"), number(t.at("degrees")), t.at("center")));
        shape = transformed(shape, move(feature.at("move")));
        const TopoDS_Shape base = shape;
        for (const Json &copy : feature.at("copies")) {
            const TopoDS_Shape instance =
                copy.contains("move") ? transformed(base, move(copy.at("move")))
                                      : transformed(base, turn(copy.at("turn").at("axis"), number(copy.at("turn").at("degrees")), copy.at("turn").at("center")));
            shape = combine(shape, instance, "add");
        }
        result = have ? combine(result, shape, feature.at("op").get<std::string>()) : shape;
        have = true;
    }
    if (!have) throw Refusal("Could not translate the geometry to STEP");
    return result;
}

std::string write_step(const TopoDS_Shape &shape) {
    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) throw Refusal("Open CASCADE could not write the STEP document");
#if OCC_VERSION_HEX >= 0x070700
    std::ostringstream stream(std::ios::out | std::ios::binary);
    if (writer.WriteStream(stream) != IFSelect_RetDone) throw Refusal("Open CASCADE could not write the STEP document");
    return stream.str();
#elif !defined(_WIN32)
    // Open CASCADE before 7.7 writes STEP to a file only: a private temporary one.
    const char *directory = std::getenv("TMPDIR");
    std::string name = std::string(directory && *directory ? directory : "/tmp") + "/kerfkite-step-XXXXXX";
    const int fd = ::mkstemp(&name[0]);
    if (fd < 0) throw Refusal("Open CASCADE could not write the STEP document");
    ::close(fd);
    const bool written = writer.Write(name.c_str()) == IFSelect_RetDone;
    std::ifstream file(name, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    ::unlink(name.c_str());
    if (!written) throw Refusal("Open CASCADE could not write the STEP document");
    return bytes;
#else
#error "kerfkite-step needs Open CASCADE 7.7 or later on Windows"
#endif
}

Json answer(const Json &request) {
    if (request.contains("version")) {
        Json out = Json::object();
        out["open_cascade"] = OCC_VERSION_COMPLETE;
        return out;
    }
    if (request.contains("inspect")) {
        try {
            return mesh_shape(read_shape(decoded(request.at("inspect"))));
        } catch (const Refusal &) {
            throw;
        } catch (const Standard_Failure &) {
            throw Refusal("Open CASCADE could not translate this STEP shape");
        } catch (const std::exception &) {
            throw Refusal("Open CASCADE could not translate this STEP shape");
        }
    }
    if (request.contains("export")) {
        const Json &what = request.at("export");
        try {
            const TopoDS_Shape shape = what.contains("faces")      ? surface_shape(what.at("faces"))
                                       : what.contains("features") ? part_shape(what.at("features"))
                                                                   : read_shape(decoded(what.at("source")));
            Json out = Json::object();
            out["step"] = encode_base64(write_step(shape));
            return out;
        } catch (const Refusal &) {
            throw;
        } catch (const Standard_Failure &) {
            throw Refusal("Could not translate the geometry to STEP");
        } catch (const std::exception &) {
            throw Refusal("Could not translate the geometry to STEP");
        }
    }
    throw Refusal("Unknown STEP request");
}

}  // namespace

int main() {
    // The answer goes to the original standard output; anything Open CASCADE prints goes to standard error.
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    const int answer_fd = _dup(_fileno(stdout));
    _dup2(_fileno(stderr), _fileno(stdout));
#else
    const int answer_fd = ::dup(STDOUT_FILENO);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
#endif
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
    const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    Json out = Json::object();
    try {
        out = answer(Json::parse(input));
    } catch (const Refusal &refusal) {
        out = Json::object();
        out["error"] = refusal.what();
    } catch (const std::exception &) {
        out = Json::object();
        out["error"] = "The STEP request could not be read";
    }
    const std::string text = out.dump() + "\n";
    size_t written = 0;
    while (written < text.size()) {
#ifdef _WIN32
        const int n = _write(answer_fd, text.data() + written, static_cast<unsigned>(text.size() - written));
#else
        const ssize_t n = ::write(answer_fd, text.data() + written, text.size() - written);
#endif
        if (n <= 0) return 1;
        written += static_cast<size_t>(n);
    }
    return 0;
}
