// The compiled extension of Studio: its functions are those of the native core (studio/_core_native.cpp) and two
// small helpers kept beside them (studio/_gcode_native.cpp). The module keeps its historical name.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *fixed_text(PyObject *, PyObject *);
PyObject *rounded_value(PyObject *, PyObject *);
PyObject *segment_rows(PyObject *, PyObject *);
PyObject *core_call(PyObject *, PyObject *);
PyObject *core_cam(PyObject *, PyObject *);
PyObject *core_cam_text(PyObject *, PyObject *);
PyObject *solid_call(PyObject *, PyObject *);
PyObject *solid_mesh(PyObject *, PyObject *);
PyObject *gpu_status(PyObject *, PyObject *);
PyObject *last_contact(PyObject *, PyObject *);

static PyMethodDef methods[] = {
    {"fixed_text", fixed_text, METH_VARARGS, "Format a float with a fixed number of decimals exactly as Python does."},
    {"rounded_value", rounded_value, METH_VARARGS, "Round a float to 0..3 decimals exactly as Python does."},
    {"segment_rows", segment_rows, METH_VARARGS, "Unpack a packed segment buffer into list rows."},
    {"core_call", core_call, METH_VARARGS, "Run a command of the native core: (command, JSON request) -> JSON answer."},
    {"core_cam", core_cam, METH_VARARGS,
     "G-code, rotary, relief or stock-preview output from the native core: (command, request, packed=False, "
     "rows=None) -> answer, or None when the request cannot cross unchanged."},
    {"core_cam_text", core_cam_text, METH_VARARGS,
     "The answer of core_cam as UTF-8 JSON text written by the core: (command, request) -> bytes, or None."},
    {"solid_call", solid_call, METH_VARARGS,
     "Run a solid request of the native core with solids kept from earlier calls: (JSON request, kept) -> (JSON answer, solid)."},
    {"solid_mesh", solid_mesh, METH_VARARGS, "The mesh of a kept solid as bytes: (solid) -> (float64 vertices, uint32 triangles)."},
    {"gpu_status", gpu_status, METH_NOARGS, "The GPU in use for relief contact, or why there is none."},
    {"last_contact", last_contact, METH_NOARGS,
     "What the calling thread's last relief contact job ran on: 'CPU, n threads' or 'GPU, device'."},
    {nullptr, nullptr, 0, nullptr}};
static PyModuleDef module = {PyModuleDef_HEAD_INIT, "_relief_native", "KerfKite's native core for Studio.", -1, methods};
PyMODINIT_FUNC PyInit__relief_native(void) { return PyModule_Create(&module); }
