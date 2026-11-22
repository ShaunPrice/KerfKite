// The design routes: studio/design_api.py and the modules it registers (assembly, cad_exchange,
// fixture_collision, program_import, import_bundle, pcb_api), answered by the core. No machine access.
#include <cmath>
#include <cstring>

#include "kerfkite/artwork_relief.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/fixture_collision.hpp"
#include "kerfkite/program_import.hpp"
#include "kerfkite/relief.hpp"
#include "kerfkite/rotary.hpp"
#include "kerfkite/solid.hpp"
#include "kerfkite/solid_cad.hpp"
#include "kerfkite/stl_heightmap.hpp"
#include "kerfkite/stock.hpp"
#include "kerfkite/tool_changes.hpp"
#include "design_extra.hpp"
#include "key_order.hpp"
#include "protocols.hpp"
#include "solid_cad_build.hpp"
#include "solid_requests.hpp"
#include "studio.hpp"
#include "surface.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;
using http::Request;
using http::Response;

const char *kNoStep = "STEP exchange needs Open CASCADE, which this build of KerfKite Studio does not include";

// body(message): the request's JSON object, or a 400 with the route's message.
json body(const Request &request, const char *message) {
    json data = json_body(request);
    if (!data.is_object()) throw ValueError(message);
    return data;
}

Response text_json(const json &value) { return Response::json(value.dump()); }

// The binary STL of a mesh, as solid_cad._encode_stl writes it: 80-byte header, then float32 normal, vertices
// and a zero attribute of each triangle.
std::string binary_stl(const std::vector<solid::Vec3> &vertices, const std::vector<std::array<int64_t, 3>> &triangles) {
    std::string out = "KerfKite Studio solid, mm";
    out.resize(80, '\0');
    const auto u32 = [&](uint32_t v) {
        for (int k = 0; k < 4; ++k) out += static_cast<char>((v >> (8 * k)) & 0xff);
    };
    const auto f32 = [&](double v) {
        const float f = static_cast<float>(v);
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        u32(bits);
    };
    u32(static_cast<uint32_t>(triangles.size()));
    for (const auto &t : triangles) {
        const solid::Vec3 &a = vertices[static_cast<size_t>(t[0])], &b = vertices[static_cast<size_t>(t[1])], &c = vertices[static_cast<size_t>(t[2])];
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length == 0) length = 1;
        nx /= length, ny /= length, nz /= length;
        f32(nx), f32(ny), f32(nz);
        for (const solid::Vec3 *p : {&a, &b, &c}) f32(p->x), f32(p->y), f32(p->z);
        out += '\0';
        out += '\0';
    }
    return out;
}

Response stl_response(const std::string &bytes, const char *filename) {
    Response r;
    r.headers.emplace_back("Content-Type", "model/stl");
    if (filename) r.headers.emplace_back("Content-Disposition", std::string("attachment; filename=\"") + filename + "\"");
    r.body = bytes;
    return r;
}

std::string model_stl(const json *model, const json &order) {
    const solid_requests::Shape shape = solid_cad_build(model, order.is_null() ? nullptr : &order);
    std::vector<solid::Vec3> vertices = shape.solid.vertices();
    std::vector<std::array<int64_t, 3>> triangles;
    for (const solid::Triangle &t : shape.solid.triangles()) triangles.push_back({int64_t(t[0]), int64_t(t[1]), int64_t(t[2])});
    return binary_stl(vertices, triangles);
}

std::string mesh_stl(const json &mesh) {
    std::vector<solid::Vec3> vertices;
    for (const json &v : mesh.at("vertices")) vertices.push_back(solid::Vec3{v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()});
    std::vector<std::array<int64_t, 3>> triangles;
    for (const json &t : mesh.at("triangles")) triangles.push_back({t.at(0).get<int64_t>(), t.at(1).get<int64_t>(), t.at(2).get<int64_t>()});
    return binary_stl(vertices, triangles);
}

// The surface request: the orders of the surface's and its patches' parameters, read from the text.
json surface_request(const json &surface, const KeyOrder *order) {
    json request = json::object();
    request["surface"] = surface;
    const KeyOrder *parameters = order ? order->member("parameters") : nullptr;
    request["order"] = surface.is_object() && member(surface, "parameters") && member(surface, "parameters")->is_object() && parameters ? parameters->order()
                       : surface.is_object() && !surface.contains("parameters")                                             ? json::array()
                                                                                                                           : json();
    const json *patches = member(surface, "patches");
    if (patches && patches->is_array()) {
        json orders = json::array();
        const KeyOrder *items = order ? order->member("patches") : nullptr;
        for (size_t i = 0; i < patches->size(); ++i) {
            const json &patch = patches->at(i);
            const KeyOrder *node = items ? items->item(i) : nullptr;
            const KeyOrder *given = node ? node->member("parameters") : nullptr;
            const json *values = member(patch, "parameters");
            orders.push_back(!patch.is_object() ? json() : !values ? json::array() : values->is_object() && given ? given->order() : json());
        }
        request["orders"] = orders;
    }
    return request;
}

}  // namespace

void add_design_routes(Studio &studio) {
    studio.add("POST", "/api/solid-cad", [](const Request &request) {
        const json data = body(request, "Supply a CAD model in the model field");
        const json *model = member(data, "model");
        if (!model || !model->is_object()) throw ValueError("Supply a CAD model in the model field");
        json core = json::object();
        core["model"] = *model;
        const json order = order_at(key_order(request.body), {"model", "parameters"});
        if (member(*model, "parameters") && member(*model, "parameters")->is_object()) core["order"] = order;
        return text_json(solid_cad_call("solid-cad", core));
    });
    studio.add("POST", "/api/cad-script", [](const Request &request) {
        json data = body(request, "Supply a CAD script");
        json core = json::object();
        for (const auto &item : data.items())
            if (item.key() != "order") core[std::string(item.key())] = item.value();
        if (member(data, "parameters") && member(data, "parameters")->is_object()) core["order"] = order_at(key_order(request.body), {"parameters"});
        return text_json(solid_cad_call("cad-script", core));
    });
    studio.add("POST", "/api/solid-cad/sketch-points", [](const Request &request) {
        const json data = body(request, "Supply sketch points and parameters");
        return text_json(sketch_points(data, order_at(key_order(request.body), {"parameters"})));
    });
    studio.add("POST", "/api/solid-cad/section", [](const Request &request) {
        const json data = body(request, "Supply a CAD model and section height");
        const json *model = member(data, "model");
        if (!model || !model->is_object()) throw ValueError("Supply a CAD model and section height");
        json core = json::object();
        core["z"] = data.contains("z") ? data.at("z") : json();
        core["model"] = *model;
        if (member(*model, "parameters") && member(*model, "parameters")->is_object()) core["order"] = order_at(key_order(request.body), {"model", "parameters"});
        return text_json(solid_cad_call("solid-cad-section", core));
    });
    studio.add("POST", "/api/solid-cad/stl", [](const Request &request) {
        const json data = body(request, "Supply a CAD model in the model field");
        const json *model = member(data, "model");
        if (!model || !model->is_object()) throw ValueError("Supply a CAD model in the model field");
        const json order = member(*model, "parameters") && member(*model, "parameters")->is_object() ? order_at(key_order(request.body), {"model", "parameters"}) : json();
        return stl_response(model_stl(model, order), "part.stl");
    });
    studio.add("GET", "/api/protocols", [](const Request &) { return text_json(protocols_json()); });
    studio.add("POST", "/api/tool-changes", [](const Request &request) {
        const json data = body(request, "Provide a project");
        const json *project = member(data, "project");
        // Every stage is a whole job: the core's own JSON text of the plan is the response.
        return text_json(plan_tool_changes(project ? *project : json()));
    });
    studio.add("POST", "/api/rotary", [](const Request &request) {
        const json data = body(request, "Provide a project and rotary setup");
        const json *project = member(data, "project");
        if (!project || !project->is_object()) throw ValueError("Provide a project and rotary setup");
        const json *options = member(data, "options");
        return text_json(generate_rotary(*project, options ? *options : json::object()));
    });
    studio.add("POST", "/api/geometry", [](const Request &request) {
        return text_json(geometry(body(request, "Select 1\xe2\x80\x93" "200 objects")));
    });
    studio.add("POST", "/api/simulate", [](const Request &request) {
        if (request.media_type() == "application/vnd.kerfkite.stock-v1") {
            std::vector<double> rows;
            const json data = stock_wire(request.body, rows);
            return text_json(simulate_stock(data, &rows));
        }
        return text_json(simulate_stock(json_body(request)));
    });
    studio.add("POST", "/api/extrude", [](const Request &request) { return text_json(extrude_artwork(body(request, "Supply closed artwork paths"))); });
    studio.add("POST", "/api/artwork-relief", [](const Request &request) {
        return text_json(artwork_relief(body(request, "Supply closed artwork paths")));
    });
    studio.add("POST", "/api/relief", [](const Request &request) {
        json data = body(request, "Expected relief settings");
        if (!truthy(member(data, "content_base64"))) return text_json(prepare_relief(data));
        const json mesh = stl_heightmap(data);
        data["heightmap"] = mesh.at("heightmap");
        json result = prepare_relief(data);
        for (const json &warning : mesh.at("warnings")) result["warnings"].push_back(warning);
        return text_json(result);
    });

    studio.add("POST", "/api/assembly/solve", [](const Request &request) {
        const json data = strict_json(request);
        const json *mesh = member(data, "mesh");
        if (!data.is_object() || (mesh && !mesh->is_boolean())) throw ValueError("Supply an assembly and a boolean mesh flag");
        json core = json::object();
        core["assembly"] = data.contains("assembly") ? data.at("assembly") : json();
        core["mesh"] = mesh ? *mesh : json(false);
        const json *assembly = member(data, "assembly");
        const json *parts = assembly ? member(*assembly, "parts") : nullptr;
        if (parts && parts->is_array()) {
            const KeyOrder text = key_order(request.body);
            const KeyOrder *items = text.member("assembly") ? text.member("assembly")->member("parts") : nullptr;
            json orders = json::array();
            for (size_t i = 0; i < parts->size(); ++i) {
                const json *model = member(parts->at(i), "model");
                const json *parameters = model ? member(*model, "parameters") : nullptr;
                const KeyOrder *node = items ? items->item(i) : nullptr;
                const KeyOrder *given = node && node->member("model") ? node->member("model")->member("parameters") : nullptr;
                orders.push_back(parameters && parameters->is_object() && given ? given->order() : json());
            }
            core["orders"] = orders;
        } else {
            core["orders"] = json();
        }
        return text_json(solid_cad_call("assembly-solve", core));
    });
    studio.add("GET", "/api/assembly/example", [](const Request &) { return text_json(example_assembly()); });
    studio.add("POST", "/api/fixture-collision", [](const Request &request) { return text_json(check_fixtures(strict_json(request))); });
    for (const char *path : {"/api/program-import", "/api/pdf-import", "/api/import-bundle"}) {
        const std::string command = std::string(path) == "/api/program-import" ? "program-import" : std::string(path) == "/api/pdf-import" ? "pdf-import" : "bundle-import";
        studio.add("POST", path, [command](const Request &request) { return text_json(program_import_call(command, strict_json(request))); });
    }
    studio.add("POST", "/api/pcb-import", [](const Request &request) { return text_json(pcb_board(json_body(request))); });

    // CAD exchange: surfaces in the core; STEP needs Open CASCADE.
    const auto exchange_payload = [](const Request &request) {
        json data = strict_json(request);
        if (!data.is_object()) throw ValueError("Supply a CAD exchange request");
        return data;
    };
    studio.add("POST", "/api/solid-cad/surface", [exchange_payload](const Request &request) {
        const json data = exchange_payload(request);
        const KeyOrder text = key_order(request.body);
        const json surface = data.contains("surface") ? data.at("surface") : json();
        return text_json(surface::mesh(surface_request(surface, text.member("surface"))));
    });
    studio.add("POST", "/api/solid-cad/step/import", [exchange_payload](const Request &request) -> Response {
        exchange_payload(request);
        throw ValueError(kNoStep);
    });
    studio.add("POST", "/api/solid-cad/step/export", [exchange_payload](const Request &request) -> Response {
        exchange_payload(request);
        throw ValueError(kNoStep);
    });
    studio.add("POST", "/api/solid-cad/exchange-stl", [exchange_payload](const Request &request) {
        const json data = exchange_payload(request);
        if (data.contains("surface")) {
            const KeyOrder text = key_order(request.body);
            return stl_response(mesh_stl(surface::mesh(surface_request(data.at("surface"), text.member("surface")))), nullptr);
        }
        if (data.contains("content")) throw ValueError(kNoStep);
        const json *model = member(data, "model");
        const json order = model && member(*model, "parameters") && member(*model, "parameters")->is_object()
                               ? order_at(key_order(request.body), {"model", "parameters"})
                               : json();
        return stl_response(model_stl(model, order), nullptr);
    });
}

}  // namespace server
}  // namespace kerfkite
