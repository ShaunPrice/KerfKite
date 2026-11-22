// Helpers the Python side keeps beside the native core's CAM (studio/_core_native.cpp, core_cam).
//
// The G-code and rotary generators are the core's own (native/core/src/cam.cpp and rotary.cpp); Studio calls
// them through core_cam, and Python remains the reference that validation/native_core_parity.py compares them
// with. What is left here: unpacking packed preview rows for the JSON boundary, and the core's exact
// fixed-point text and round(), exposed so that tests can compare them with Python's formatting.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <cmath>
#include <cstring>
#include <string>

#include "kerfkite/number_text.hpp"

namespace {

using kerfkite::text::fixed;
using kerfkite::text::rounded;

struct Reference {
    PyObject *object;
    explicit Reference(PyObject *o = nullptr) : object(o) {}
    ~Reference() { Py_XDECREF(object); }
    PyObject *release() {
        PyObject *o = object;
        object = nullptr;
        return o;
    }
};

}  // namespace

// segment_rows(rows) -> list of [x, y, nx, ny, z, power or None, start] from a packed buffer.
PyObject *segment_rows(PyObject *, PyObject *args) {
    Py_buffer rows;
    if (!PyArg_ParseTuple(args, "y*", &rows)) return nullptr;
    struct Release {
        Py_buffer *view;
        ~Release() { PyBuffer_Release(view); }
    } release = {&rows};
    const Py_ssize_t size = 7 * static_cast<Py_ssize_t>(sizeof(double));
    if (rows.len % size) Py_RETURN_NONE;
    const Py_ssize_t count = rows.len / size;
    Reference list(PyList_New(count));
    if (!list.object) return nullptr;
    const char *data = static_cast<const char *>(rows.buf);
    for (Py_ssize_t index = 0; index < count; index++) {
        double row[7];
        std::memcpy(row, data + index * size, sizeof row);
        PyObject *fields = PyList_New(7);
        if (!fields) return nullptr;
        PyList_SET_ITEM(list.object, index, fields);
        for (int field = 0; field < 7; field++) {
            PyObject *value;
            if (field == 5 && std::isnan(row[5])) {
                Py_INCREF(Py_None);
                value = Py_None;
            } else {
                value = PyFloat_FromDouble(row[field]);
                if (!value) return nullptr;
            }
            PyList_SET_ITEM(fields, field, value);
        }
    }
    return list.release();
}

// fixed_text(value, decimals) -> str or None. Exposes the formatter so tests can compare it with Python's.
PyObject *fixed_text(PyObject *, PyObject *args) {
    double value;
    int decimals;
    if (!PyArg_ParseTuple(args, "di", &value, &decimals)) return nullptr;
    if (decimals < 0 || decimals > 9) Py_RETURN_NONE;
    std::string text;
    if (!fixed(text, value, decimals)) Py_RETURN_NONE;
    return PyUnicode_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size()));
}

// rounded_value(value, decimals) -> float or None. Python's round(value, decimals) for 0..9 decimals.
PyObject *rounded_value(PyObject *, PyObject *args) {
    double value, result;
    int decimals;
    if (!PyArg_ParseTuple(args, "di", &value, &decimals)) return nullptr;
    if (decimals < 0 || decimals > 9 || !rounded(value, decimals, result)) Py_RETURN_NONE;
    return PyFloat_FromDouble(result);
}
