// The native core's commands for Python: levelling, circuit boards, polygons and solids take JSON text and
// give JSON text; G-code and rotary output (core_cam) take and give Python objects, which spares a job of
// 200,000 moves being written out as text and read back.
//
// Unlike the other functions of this extension there is no second implementation in C++ behind these: probe
// levelling, board import, the offset, pocket and fill operations, the polygon and solid kernels and the CAM
// are written once, in native/core, and Studio calls them here.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/leveling.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/relief.hpp"
#include "kerfkite/rotary.hpp"
#include "kerfkite/solid_command.hpp"
#include "kerfkite/stock.hpp"

PyObject *core_call(PyObject *, PyObject *args) {
    const char *command_text, *request_text;
    Py_ssize_t request_size;
    if (!PyArg_ParseTuple(args, "ss#", &command_text, &request_text, &request_size)) return nullptr;
    const std::string command = command_text, request(request_text, static_cast<size_t>(request_size));
    std::string answer, failure;
    bool rejected = false;
    // Nothing below touches Python objects, so other threads may run meanwhile.
    Py_BEGIN_ALLOW_THREADS
    try {
        const kerfkite::Json parsed = kerfkite::Json::parse(request);
        answer = (kerfkite::pcb_command(command)         ? kerfkite::pcb_call(command, parsed)
                  : command == "solid"                   ? kerfkite::solid_call(parsed)
                  : kerfkite::polygon_command(command)   ? kerfkite::polygon_call(command, parsed)
                  : kerfkite::operation_command(command) ? kerfkite::operation_call(command, parsed)
                                                         : kerfkite::levelling_call(command, parsed))
                     .dump();
    } catch (const kerfkite::ValueError &error) {
        rejected = true;
        failure = error.what();
    } catch (const kerfkite::Json::exception &) {
        rejected = true;
        failure = "The request is not valid JSON";
    } catch (const std::exception &error) {
        failure = error.what();
    }
    Py_END_ALLOW_THREADS
    if (rejected) {
        PyErr_SetString(PyExc_ValueError, failure.c_str());
        return nullptr;
    }
    if (!failure.empty()) {
        PyErr_SetString(PyExc_RuntimeError, failure.c_str());
        return nullptr;
    }
    return PyUnicode_FromStringAndSize(answer.data(), static_cast<Py_ssize_t>(answer.size()));
}

namespace {

// A reference released when it goes out of scope.
struct Owned {
    PyObject *object;
    explicit Owned(PyObject *given = nullptr) noexcept : object(given) {}
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    ~Owned() { Py_XDECREF(object); }
    PyObject *release() noexcept {
        PyObject *given = object;
        object = nullptr;
        return given;
    }
};

// A Python value as the core's JSON, as json.dumps would carry it: None, a bool, an int within 64 bits, a
// finite float, a str, and lists and dicts with str keys, each of exactly that type. False for anything else
// (a tuple, a subclass, a non-finite float, a key that is not text), which JSON would change or cannot carry,
// so the reference must answer it. May throw std::bad_alloc.
bool to_core(PyObject *value, kerfkite::Json &out, int depth) {
    if (depth > kerfkite::Json::MAX_DEPTH) return false;
    if (value == Py_None) {
        out = kerfkite::Json();
        return true;
    }
    if (value == Py_True || value == Py_False) {
        out = kerfkite::Json(value == Py_True);
        return true;
    }
    if (PyFloat_CheckExact(value)) {
        const double number = PyFloat_AS_DOUBLE(value);
        if (!std::isfinite(number)) return false;
        out = kerfkite::Json(number);
        return true;
    }
    if (PyLong_CheckExact(value)) {
        int overflow = 0;
        const long long number = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (overflow || (number == -1 && PyErr_Occurred())) {
            PyErr_Clear();
            return false;
        }
        out = kerfkite::Json(static_cast<int64_t>(number));
        return true;
    }
    if (PyUnicode_CheckExact(value)) {
        Py_ssize_t size = 0;
        const char *text = PyUnicode_AsUTF8AndSize(value, &size);
        if (!text) {  // a lone surrogate: not UTF-8
            PyErr_Clear();
            return false;
        }
        out = kerfkite::Json(std::string(text, static_cast<size_t>(size)));
        return true;
    }
    if (PyList_CheckExact(value)) {
        const Py_ssize_t count = PyList_GET_SIZE(value);
        kerfkite::Json::Array items(static_cast<size_t>(count));
        for (Py_ssize_t i = 0; i < count; ++i)
            if (!to_core(PyList_GET_ITEM(value, i), items[static_cast<size_t>(i)], depth + 1)) return false;
        out = kerfkite::Json(std::move(items));
        return true;
    }
    if (PyDict_CheckExact(value)) {
        kerfkite::Json::Object members;
        members.reserve(static_cast<size_t>(PyDict_GET_SIZE(value)));
        PyObject *key = nullptr, *item = nullptr;
        Py_ssize_t position = 0;
        while (PyDict_Next(value, &position, &key, &item)) {
            if (!PyUnicode_CheckExact(key)) return false;
            Py_ssize_t size = 0;
            const char *name = PyUnicode_AsUTF8AndSize(key, &size);
            if (!name) {
                PyErr_Clear();
                return false;
            }
            members.emplace_back(std::string(name, static_cast<size_t>(size)), kerfkite::Json());
            if (!to_core(item, members.back().second, depth + 1)) return false;
        }
        out = kerfkite::Json::object(std::move(members));
        return true;
    }
    return false;
}

// The core's JSON as Python objects, as json.loads gives them.
PyObject *from_core(const kerfkite::Json &value) {
    using Type = kerfkite::Json::Type;
    switch (value.type()) {
    case Type::null:
        Py_RETURN_NONE;
    case Type::boolean:
        return PyBool_FromLong(value.get<bool>());
    case Type::number_integer:
        return PyLong_FromLongLong(value.get<int64_t>());
    case Type::number_unsigned:
        return PyLong_FromUnsignedLongLong(value.get<uint64_t>());
    case Type::number_float:
        return PyFloat_FromDouble(value.get<double>());
    case Type::string: {
        const std::string &text = value.get_ref<const std::string &>();
        return PyUnicode_DecodeUTF8(text.data(), static_cast<Py_ssize_t>(text.size()), "strict");
    }
    case Type::array: {
        Owned list(PyList_New(static_cast<Py_ssize_t>(value.size())));
        if (!list.object) return nullptr;
        Py_ssize_t index = 0;
        for (const kerfkite::Json &entry : value) {
            PyObject *item = from_core(entry);
            if (!item) return nullptr;
            PyList_SET_ITEM(list.object, index++, item);
        }
        return list.release();
    }
    case Type::object: {
        Owned dict(PyDict_New());
        if (!dict.object) return nullptr;
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string &name = it.key();
            Owned key(PyUnicode_DecodeUTF8(name.data(), static_cast<Py_ssize_t>(name.size()), "strict"));
            Owned item(from_core(it.value()));
            if (!key.object || !item.object || PyDict_SetItem(dict.object, key.object, item.object) < 0) return nullptr;
        }
        return dict.release();
    }
    default:
        PyErr_SetString(PyExc_RuntimeError, "The native core gave a value that is not JSON");
        return nullptr;
    }
}

PyObject *text_list(const std::vector<std::string> &lines) {
    Owned list(PyList_New(static_cast<Py_ssize_t>(lines.size())));
    if (!list.object) return nullptr;
    for (size_t i = 0; i < lines.size(); ++i) {
        PyObject *text = PyUnicode_DecodeUTF8(lines[i].data(), static_cast<Py_ssize_t>(lines[i].size()), "strict");
        if (!text) return nullptr;
        PyList_SET_ITEM(list.object, static_cast<Py_ssize_t>(i), text);
    }
    return list.release();
}

// A list of floats, or of lists of floats nested as the values are.
template <class Row> PyObject *float_row(const Row &row) {
    Owned list(PyList_New(static_cast<Py_ssize_t>(row.size())));
    if (!list.object) return nullptr;
    for (size_t i = 0; i < row.size(); ++i) {
        PyObject *number = PyFloat_FromDouble(row[i]);
        if (!number) return nullptr;
        PyList_SET_ITEM(list.object, static_cast<Py_ssize_t>(i), number);
    }
    return list.release();
}

template <class Rows> PyObject *float_rows(const Rows &rows) {
    Owned list(PyList_New(static_cast<Py_ssize_t>(rows.size())));
    if (!list.object) return nullptr;
    for (size_t i = 0; i < rows.size(); ++i) {
        PyObject *row = float_row(rows[i]);
        if (!row) return nullptr;
        PyList_SET_ITEM(list.object, static_cast<Py_ssize_t>(i), row);
    }
    return list.release();
}

// The same double, bit for bit: 0.0 and -0.0 are different floats to Python.
bool same(double a, double b) {
    uint64_t x, y;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

// How the preview rows are given: as studio.cam writes them, packed into bytes, or as the rotary post sees them.
enum class Rows { lists, packed, rotary };

// The preview segments as studio.cam writes them: rows of [x, y, nx, ny, z, power or None, start z], with the
// integer 0 where the reference has one; packed, the rows as native doubles with NaN for no power, which
// studio.cam_moves.PackedSegments.from_native wraps; or for the rotary post, with the rows the reference
// packs (Segment::packed) holding floats throughout. A row starts where the one before it ended, so its
// start takes the float objects of that end rather than new ones of the same value.
PyObject *segment_list(const std::vector<kerfkite::Segment> &segments, Rows form) {
    if (form == Rows::packed) {
        std::vector<double> rows;
        rows.reserve(segments.size() * 7);
        for (const kerfkite::Segment &s : segments) {
            const double power = s.has_power ? (s.whole_power ? 0.0 : s.power) : std::nan("");
            const double end = s.planar || s.whole_end ? 0.0 : s.end_z, start = s.planar || s.whole_start ? 0.0 : s.start_z;
            rows.insert(rows.end(), {s.x, s.y, s.nx, s.ny, end, power, start});
        }
        return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(rows.data()),
                                         static_cast<Py_ssize_t>(rows.size() * sizeof(double)));
    }
    Owned list(PyList_New(static_cast<Py_ssize_t>(segments.size())));
    if (!list.object) return nullptr;
    // A float for this value: the one given if it is the same double, otherwise a new one.
    const auto number = [](double value, PyObject *given) -> PyObject * {
        if (given && PyFloat_CheckExact(given) && same(PyFloat_AS_DOUBLE(given), value)) {
            Py_INCREF(given);
            return given;
        }
        return PyFloat_FromDouble(value);
    };
    PyObject *last = nullptr;  // the row before, owned by the list
    for (size_t i = 0; i < segments.size(); ++i) {
        const kerfkite::Segment &s = segments[i];
        // The reference's integer 0, which a packed row holds as the float 0.0.
        const bool floats = form == Rows::rotary && s.packed;
        const auto zero = [floats]() { return floats ? PyFloat_FromDouble(0.0) : PyLong_FromLong(0); };
        Owned row(PyList_New(7));
        if (!row.object) return nullptr;
        PyObject *const before_x = last ? PyList_GET_ITEM(last, 2) : nullptr;
        PyObject *const before_y = last ? PyList_GET_ITEM(last, 3) : nullptr;
        PyObject *const before_z = last ? PyList_GET_ITEM(last, 4) : nullptr;
        PyObject *values[7] = {
            number(s.x, before_x),
            number(s.y, before_y),
            PyFloat_FromDouble(s.nx),
            PyFloat_FromDouble(s.ny),
            s.planar || s.whole_end ? zero() : PyFloat_FromDouble(s.end_z),
            nullptr,
            s.planar || s.whole_start ? zero() : number(s.start_z, before_z),
        };
        if (s.has_power) {
            values[5] = s.whole_power ? zero() : PyFloat_FromDouble(s.power);
        } else {
            Py_INCREF(Py_None);
            values[5] = Py_None;
        }
        bool failed = false;
        for (Py_ssize_t k = 0; k < 7; ++k) {
            if (!values[k]) failed = true;
            PyList_SET_ITEM(row.object, k, values[k]);  // the list releases what it holds, nulls included
        }
        if (failed) return nullptr;
        last = row.object;
        PyList_SET_ITEM(list.object, static_cast<Py_ssize_t>(i), row.release());
    }
    return list.release();
}

bool put(PyObject *dict, const char *name, PyObject *value) {
    if (!value) return false;
    const int result = PyDict_SetItemString(dict, name, value);
    Py_DECREF(value);
    return result == 0;
}

// The job as studio.cam.generate returns it, its members in the reference's order.
PyObject *job_object(const kerfkite::Job &job, Rows form) {
    Owned dict(PyDict_New());
    if (!dict.object) return nullptr;
    PyObject *d = dict.object;
    if (!put(d, "lines", text_list(job.lines)) || !put(d, "segments", segment_list(job.segments, form)) ||
        !put(d, "cut_length", PyFloat_FromDouble(job.cut_length)) ||
        !put(d, "cut_minutes", PyFloat_FromDouble(job.cut_minutes)) ||
        !put(d, "mode", PyUnicode_DecodeUTF8(job.mode.data(), static_cast<Py_ssize_t>(job.mode.size()), "strict")) ||
        !put(d, "bounds", float_row(job.bounds)) || !put(d, "warnings", text_list(job.warnings)))
        return nullptr;
    if (job.operations && !put(d, "operations", PyLong_FromSize_t(job.operations))) return nullptr;
    if (job.drilled && !put(d, "drill_holes", float_rows(job.drill_holes))) return nullptr;
    if (job.has_simulation_tool) {
        Owned tool(PyDict_New());
        if (!tool.object || !put(tool.object, "diameter", PyFloat_FromDouble(job.simulation_tool.diameter)) ||
            !put(tool.object, "shape",
                 PyUnicode_DecodeUTF8(job.simulation_tool.shape.data(),
                                      static_cast<Py_ssize_t>(job.simulation_tool.shape.size()), "strict")) ||
            !put(tool.object, "angle", PyFloat_FromDouble(job.simulation_tool.angle)) ||
            !put(d, "simulation_tool", tool.release()))
            return nullptr;
    }
    if (job.prepared) {
        Owned paths(PyList_New(static_cast<Py_ssize_t>(job.prepared_paths.size())));
        if (!paths.object) return nullptr;
        for (size_t i = 0; i < job.prepared_paths.size(); ++i) {
            PyObject *path = float_rows(job.prepared_paths[i]);
            if (!path) return nullptr;
            PyList_SET_ITEM(paths.object, static_cast<Py_ssize_t>(i), path);
        }
        if (!put(d, "prepared_paths", paths.release())) return nullptr;
        PyObject *depths = nullptr;
        if (job.has_prepared_depths) {
            depths = float_rows(job.prepared_depths);
        } else {
            Py_INCREF(Py_None);
            depths = Py_None;
        }
        if (!put(d, "prepared_depths", depths)) return nullptr;
    }
    return dict.release();
}

}  // namespace

// core_cam(command, request, packed=False, rows=None) -> the answer as Python objects, or None
//   "generate": request is a project; the job as studio.cam.generate returns it, its segments packed when
//               packed is true.
//   "rotary": request is {"project", "options"}; "rotary-wrap": {"job", "options"}: the rotary job as
//               studio.rotary.generate_rotary and wrap_job return it.
//   "relief": studio.relief.prepare_relief; "stock": studio.simulation.simulate, whose segments may come as
//               rows, bytes of seven native doubles a segment as packed preview rows hold them.
// None when the request cannot cross to the core unchanged (see to_core), so that the reference answers it.
// A rejected request raises ValueError with the reference's message; one the core does not reproduce raises
// NotImplementedError naming what it lacks. The work runs without the GIL.
PyObject *core_cam(PyObject *, PyObject *args) {
    const char *command_text = nullptr;
    PyObject *request = nullptr, *rows_given = nullptr;
    int packed = 0;
    if (!PyArg_ParseTuple(args, "sO|pO", &command_text, &request, &packed, &rows_given)) return nullptr;
    const std::string command = command_text;
    const bool generating = command == "generate";
    if (!generating && !kerfkite::rotary_command(command) && !kerfkite::relief_command(command) &&
        !kerfkite::stock_command(command)) {
        PyErr_SetString(PyExc_ValueError, "Unknown CAM command");
        return nullptr;
    }
    kerfkite::Json given;
    std::vector<double> rows;
    const bool has_rows = rows_given && rows_given != Py_None;
    if (has_rows && (!kerfkite::stock_command(command) || !PyBytes_Check(rows_given) ||
                     PyBytes_GET_SIZE(rows_given) % Py_ssize_t(7 * sizeof(double)))) {
        PyErr_SetString(PyExc_ValueError, "Rows must be bytes of seven doubles a segment, for the stock preview");
        return nullptr;
    }
    // No C++ exception may leave this function: Python's caller is C.
    try {
        if (!to_core(request, given, 0)) Py_RETURN_NONE;
        if (has_rows) {
            rows.resize(size_t(PyBytes_GET_SIZE(rows_given)) / sizeof(double));
            std::memcpy(rows.data(), PyBytes_AS_STRING(rows_given), rows.size() * sizeof(double));
        }
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    }
    kerfkite::Job job;
    kerfkite::RotaryJob rotary;
    kerfkite::Json answer;
    std::string failure;
    enum { done, rejected, declined, failed, memory } outcome = done;
    Py_BEGIN_ALLOW_THREADS
    try {
        if (generating) {
            job = kerfkite::generate(given);
        } else if (kerfkite::relief_command(command)) {
            answer = kerfkite::prepare_relief(given);
        } else if (kerfkite::stock_command(command)) {
            answer = kerfkite::simulate_stock(given, has_rows ? &rows : nullptr);
        } else if (command == "rotary") {
            // rotary_call's reading of the request, kept apart so that the job need not be written as JSON.
            if (!given.is_object()) throw kerfkite::ValueError("Supply a project and rotary setup");
            const kerfkite::Json none, *project = given.contains("project") ? &given.at("project") : &none;
            const kerfkite::Json empty = kerfkite::Json::object();
            const kerfkite::Json *options = given.contains("options") ? &given.at("options") : &empty;
            rotary = kerfkite::rotary_job(*project, *options);
        } else {
            answer = kerfkite::rotary_call(command, given);
        }
    } catch (const kerfkite::ValueError &error) {
        outcome = rejected;
        failure = error.what();
    } catch (const kerfkite::NotPorted &error) {
        outcome = declined;
        failure = error.what();
    } catch (const std::bad_alloc &) {
        outcome = memory;
    } catch (const std::exception &error) {
        outcome = failed;
        failure = error.what();
    }
    Py_END_ALLOW_THREADS
    switch (outcome) {
    case rejected:
        PyErr_SetString(PyExc_ValueError, failure.c_str());
        return nullptr;
    case declined:
        PyErr_SetString(PyExc_NotImplementedError, failure.c_str());
        return nullptr;
    case memory:
        return PyErr_NoMemory();
    case failed:
        PyErr_SetString(PyExc_RuntimeError, failure.empty() ? "The native core failed" : failure.c_str());
        return nullptr;
    case done:
        break;
    }
    // A job is up to a million new objects, none of which can be part of a cycle: collecting cycles while they
    // are made only walks them again and again.
    struct Collection {
        int was = PyGC_Disable();
        ~Collection() {
            if (was) PyGC_Enable();
        }
    } paused;
    try {
        if (generating) return job_object(job, packed ? Rows::packed : Rows::lists);
        if (command != "rotary") return from_core(answer);
        Owned result(job_object(rotary.planar, Rows::rotary));
        if (!result.object || !put(result.object, "lines", text_list(rotary.lines)) ||
            !put(result.object, "warnings", from_core(rotary.warnings)) ||
            !put(result.object, "rotary", from_core(rotary.rotary)))
            return nullptr;
        return result.release();
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    } catch (const std::exception &error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    }
}

// core_cam_text(command, request) -> bytes, or None
// The answer of core_cam as UTF-8 JSON text written by the core, for a web response to send as it is: names
// in byte order and numbers as Python writes them, as Flask's jsonify gives them, with text that is not ASCII
// left as it is rather than escaped. None, ValueError and NotImplementedError as for core_cam.
PyObject *core_cam_text(PyObject *, PyObject *args) {
    const char *command_text = nullptr;
    PyObject *request = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &command_text, &request)) return nullptr;
    const std::string command = command_text;
    if (command != "generate" && !kerfkite::rotary_command(command) && !kerfkite::relief_command(command) &&
        !kerfkite::stock_command(command)) {
        PyErr_SetString(PyExc_ValueError, "Unknown CAM command");
        return nullptr;
    }
    kerfkite::Json given;
    try {
        if (!to_core(request, given, 0)) Py_RETURN_NONE;
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    }
    std::string text, failure;
    enum { done, rejected, declined, failed, memory } outcome = done;
    Py_BEGIN_ALLOW_THREADS
    try {
        text = (command == "generate"                     ? kerfkite::to_json(kerfkite::generate(given))
                : kerfkite::relief_command(command) ? kerfkite::prepare_relief(given)
                : kerfkite::stock_command(command)  ? kerfkite::simulate_stock(given)
                                                     : kerfkite::rotary_call(command, given))
                   .dump();
    } catch (const kerfkite::ValueError &error) {
        outcome = rejected;
        failure = error.what();
    } catch (const kerfkite::NotPorted &error) {
        outcome = declined;
        failure = error.what();
    } catch (const std::bad_alloc &) {
        outcome = memory;
    } catch (const std::exception &error) {
        outcome = failed;
        failure = error.what();
    }
    Py_END_ALLOW_THREADS
    switch (outcome) {
    case rejected:
        PyErr_SetString(PyExc_ValueError, failure.c_str());
        return nullptr;
    case declined:
        PyErr_SetString(PyExc_NotImplementedError, failure.c_str());
        return nullptr;
    case memory:
        return PyErr_NoMemory();
    case failed:
        PyErr_SetString(PyExc_RuntimeError, failure.empty() ? "The native core failed" : failure.c_str());
        return nullptr;
    case done:
        break;
    }
    return PyBytes_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size()));
}

// gpu_status(): the GPU the relief contact uses, or why there is none (looking for it if that has not been done);
// last_contact(): what the calling thread's last contact ran on.
PyObject *gpu_status(PyObject *, PyObject *) {
    std::string text;
    Py_BEGIN_ALLOW_THREADS
    text = kerfkite::relief_gpu_status();
    Py_END_ALLOW_THREADS
    return PyUnicode_FromString(text.c_str());
}

PyObject *last_contact(PyObject *, PyObject *) { return PyUnicode_FromString(kerfkite::last_relief_contact().c_str()); }

// A solid kept between calls. Python holds it as a capsule and hands it back in the `kept` list of a later
// request; the capsule owns a copy of the Solid, which shares the mesh and never changes it, so any
// number of threads may use one solid at once.
static const char *const kSolidCapsule = "kerfkite.solid";

static void release_solid(PyObject *capsule) {
    delete static_cast<kerfkite::solid::Solid *>(PyCapsule_GetPointer(capsule, kSolidCapsule));
}

// solid_call(request, kept=()) -> (answer, solid): the "solid" command, where a node {"kept": i} stands for
// kept[i] and the solid of "shape" comes back to be kept in turn.
PyObject *solid_call(PyObject *, PyObject *args) {
    const char *request_text;
    Py_ssize_t request_size;
    PyObject *kept_given = nullptr;
    if (!PyArg_ParseTuple(args, "s#|O", &request_text, &request_size, &kept_given)) return nullptr;
    std::vector<kerfkite::solid::Solid> kept;
    if (kept_given && kept_given != Py_None) {
        PyObject *sequence = PySequence_Fast(kept_given, "The kept solids must be a sequence");
        if (!sequence) return nullptr;
        for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(sequence); ++i) {
            void *solid = PyCapsule_GetPointer(PySequence_Fast_GET_ITEM(sequence, i), kSolidCapsule);
            if (!solid) {
                Py_DECREF(sequence);
                return nullptr;
            }
            // No C++ exception may leave this function: Python's caller is C.
            try {
                kept.push_back(*static_cast<kerfkite::solid::Solid *>(solid));
            } catch (const std::exception &) {
                Py_DECREF(sequence);
                return PyErr_NoMemory();
            }
        }
        Py_DECREF(sequence);
    }
    std::string request;
    try {
        request.assign(request_text, static_cast<size_t>(request_size));
    } catch (const std::exception &) {
        return PyErr_NoMemory();
    }
    kerfkite::solid::Solid built;
    std::string answer, failure;
    bool rejected = false;
    Py_BEGIN_ALLOW_THREADS
    try {
        answer = kerfkite::solid_call(kerfkite::Json::parse(request), kept, &built).dump();
    } catch (const kerfkite::ValueError &error) {
        rejected = true;
        failure = error.what();
    } catch (const kerfkite::Json::exception &) {
        rejected = true;
        failure = "The request is not valid JSON";
    } catch (const std::exception &error) {
        failure = error.what();
        if (failure.empty()) failure = "The solid kernel failed";
    }
    Py_END_ALLOW_THREADS
    if (rejected) {
        PyErr_SetString(PyExc_ValueError, failure.c_str());
        return nullptr;
    }
    if (!failure.empty()) {
        PyErr_SetString(PyExc_RuntimeError, failure.c_str());
        return nullptr;
    }
    kerfkite::solid::Solid *owned = new (std::nothrow) kerfkite::solid::Solid(built);
    PyObject *capsule = owned ? PyCapsule_New(owned, kSolidCapsule, release_solid) : PyErr_NoMemory();
    if (!capsule) {
        delete owned;
        return nullptr;
    }
    PyObject *text = PyUnicode_FromStringAndSize(answer.data(), static_cast<Py_ssize_t>(answer.size()));
    if (!text) {
        Py_DECREF(capsule);
        return nullptr;
    }
    return Py_BuildValue("(NN)", text, capsule);
}

// solid_mesh(solid) -> (vertices, triangles): the mesh of a kept solid as two byte strings, float64 x, y, z
// in millimetres for each vertex and uint32 a, b, c for each triangle, in the machine's own byte order.
// They are the numbers of the "mesh" of an answer; a mesh of thousands of triangles need not be written
// out as text and read back to cross from the kernel to numpy.
PyObject *solid_mesh(PyObject *, PyObject *args) {
    PyObject *capsule = nullptr;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return nullptr;
    void *pointer = PyCapsule_GetPointer(capsule, kSolidCapsule);
    if (!pointer) return nullptr;
    static_assert(sizeof(kerfkite::solid::Vec3) == 3 * sizeof(double), "a vertex is three doubles");
    static_assert(sizeof(kerfkite::solid::Triangle) == 3 * sizeof(std::uint32_t), "a triangle is three 32-bit numbers");
    const kerfkite::solid::Solid &solid = *static_cast<kerfkite::solid::Solid *>(pointer);
    std::vector<kerfkite::solid::Vec3> vertices;
    // No C++ exception may leave this function: Python's caller is C.
    try {
        vertices = solid.vertices();
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    } catch (const std::exception &error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "The solid kernel failed");
        return nullptr;
    }
    const std::vector<kerfkite::solid::Triangle> &triangles = solid.triangles();
    PyObject *points = PyBytes_FromStringAndSize(reinterpret_cast<const char *>(vertices.data()), static_cast<Py_ssize_t>(vertices.size() * sizeof(kerfkite::solid::Vec3)));
    if (!points) return nullptr;
    PyObject *corners =
        PyBytes_FromStringAndSize(reinterpret_cast<const char *>(triangles.data()), static_cast<Py_ssize_t>(triangles.size() * sizeof(kerfkite::solid::Triangle)));
    if (!corners) {
        Py_DECREF(points);
        return nullptr;
    }
    return Py_BuildValue("(NN)", points, corners);
}
