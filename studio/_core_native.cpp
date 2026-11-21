// The native core's levelling, circuit board, polygon and solid commands for Python: JSON text in, JSON
// text out.
//
// Unlike the other functions of this extension there is no second implementation in C++ behind this one:
// probe levelling, board import, the offset, pocket and fill operations, the polygon kernel and the solid
// kernel are written once, in native/core, and Studio calls them here.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <new>
#include <string>
#include <vector>

#include "kerfkite/leveling.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/solid_command.hpp"

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
            kept.push_back(*static_cast<kerfkite::solid::Solid *>(solid));
        }
        Py_DECREF(sequence);
    }
    const std::string request(request_text, static_cast<size_t>(request_size));
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
