// The native core's levelling, circuit board and polygon operation commands for Python: JSON text in,
// JSON text out.
//
// Unlike the other functions of this extension there is no second implementation in C++ behind this one:
// probe levelling, board import and the offset, pocket and fill operations are written once, in
// native/core, and Studio calls them here.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string>

#include "kerfkite/leveling.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"

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
