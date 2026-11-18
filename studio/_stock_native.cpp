// Bounded material-removal grid. Python owns job validation and the response envelope.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace {
constexpr int fields = 7;
constexpr int segment_limit = 200000;
constexpr int visit_limit = 8000000;

bool read_number(PyObject* value, double& out) {
    // Match float(value), including numeric strings in legacy JSON.
    if (PyFloat_Check(value)) {
        out = PyFloat_AS_DOUBLE(value);
        return true;
    }
    PyObject* number = PyNumber_Float(value);
    if (!number) return false;
    out = PyFloat_AS_DOUBLE(number);
    Py_DECREF(number);
    return true;
}

bool in_range(double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
}

const char* rasterize(const char* data, Py_ssize_t count, int n, double width, double height,
                      double x, double y, double radius, int shape, double angle, double stock,
                      std::vector<double>& grid) {
    const double dx = width / (n - 1), dy = height / (n - 1);
    const double slope = std::tan(angle * (std::acos(-1.0) / 180.0) / 2);
    int visits = 0;
    for (Py_ssize_t k = 0; k < count; ++k) {
        // Bytes from Python need not have native double alignment.
        double move[fields];
        std::memcpy(move, data + k * sizeof(move), sizeof(move));
        const double ax = move[0] - x, ay = move[1] - y, bx = move[2] - x, by = move[3] - y;
        const double z = move[4], start = move[6];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) ||
            !std::isfinite(z) || !std::isfinite(start)) return "Non-finite segment";
        if (std::min(z, start) < -stock) return "Cut exceeds simulated stock thickness";

        // Intersect in floating point first. Inputs may be far outside the stock.
        const double lo_x = (std::min(ax,bx)-radius)/dx, hi_x = (std::max(ax,bx)+radius)/dx;
        const double lo_y = (std::min(ay,by)-radius)/dy, hi_y = (std::max(ay,by)+radius)/dy;
        if (!std::isfinite(lo_x) || !std::isfinite(hi_x) || !std::isfinite(lo_y) || !std::isfinite(hi_y))
            return "Segment exceeds numeric preview range";
        const double x0 = std::max(0., std::floor(lo_x)), x1 = std::min(double(n-1), std::ceil(hi_x));
        const double y0 = std::max(0., std::floor(lo_y)), y1 = std::min(double(n-1), std::ceil(hi_y));
        if (x1 < x0 || y1 < y0) continue;
        visits += (int(x1) - int(x0) + 1) * (int(y1) - int(y0) + 1);
        if (visits > visit_limit) return "Simulation too large; lower preview resolution";
        const double vx = bx - ax, vy = by - ay, square = vx * vx + vy * vy;
        for (int j = int(y0); j <= int(y1); ++j) {
            for (int i = int(x0); i <= int(x1); ++i) {
                const double rx = i * dx - ax, ry = j * dy - ay;
                const double t = square ? std::max(0., std::min(1., (rx*vx + ry*vy) / square)) : 0.;
                const double distance = std::hypot(rx - t*vx, ry - t*vy);
                if (distance > radius) continue;
                double sag = 0;
                if (shape == 1) sag = radius - std::sqrt(std::max(0., radius*radius - distance*distance));
                if (shape >= 2) sag = distance / slope;
                const double tip = square ? start + t * (z - start) : std::min(z, start);
                double& cell = grid[j*n+i];
                cell = std::min(cell, tip + sag);
            }
        }
    }
    return nullptr;
}
} // namespace

PyObject* stock_heights(PyObject*, PyObject* args) {
    PyObject* source;
    int resolution, shape;
    double width, height, x, y, diameter, angle, stock;
    if (!PyArg_ParseTuple(args, "Oidddddidd", &source, &resolution, &width, &height, &x, &y,
                          &diameter, &shape, &angle, &stock)) return nullptr;
    if (resolution < 16 || resolution > 96 || !in_range(width,.1,2000) || !in_range(height,.1,2000) ||
        !in_range(x,0,2000) || !in_range(y,0,2000) || !in_range(diameter,.01,100) ||
        shape < 0 || shape > 3 || !in_range(angle,10,170) || !in_range(stock,.01,200)) {
        PyErr_SetString(PyExc_ValueError, "Invalid stock simulation parameters");
        return nullptr;
    }
    try {
        std::vector<double> owned;
        const char* data = nullptr;
        Py_ssize_t count;
        if (PyBytes_Check(source)) {
            const Py_ssize_t stride = fields * sizeof(double), size = PyBytes_GET_SIZE(source);
            count = size / stride;
            if (size % stride || count > segment_limit) {
                PyErr_SetString(PyExc_ValueError, "Invalid packed segment count"); return nullptr;
            }
            data = PyBytes_AS_STRING(source);
        } else {
            if (!PyList_Check(source) || (count = PyList_GET_SIZE(source)) > segment_limit) {
                PyErr_SetString(PyExc_ValueError, "Invalid segment count"); return nullptr;
            }
            owned.resize(count * fields);
            // Keep the GIL while reading Python rows. Only private or immutable memory
            // is inspected while rasterizing without the GIL.
            for (Py_ssize_t i = 0; i < count; ++i) {
                PyObject* row = PyList_GetItem(source, i);
                if (!row) return nullptr;
                if (!PyList_Check(row) || PyList_GET_SIZE(row) < 5) {
                    PyErr_SetString(PyExc_ValueError, "Invalid preview segment"); return nullptr;
                }
                // User-defined __float__ can mutate the source during conversion.
                PyObject* items = PyList_GetSlice(row, 0, fields);
                if (!items) return nullptr;
                double* move = owned.data() + i*fields;
                bool ok = true;
                for (int j = 0; j < 5 && ok; ++j)
                    ok = read_number(PyList_GET_ITEM(items,j), move[j]);
                move[6] = move[4];
                if (ok && PyList_GET_SIZE(items) > 6) ok = read_number(PyList_GET_ITEM(items,6), move[6]);
                Py_DECREF(items);
                if (!ok) return nullptr;
            }
            data = reinterpret_cast<const char*>(owned.data());
        }
        std::vector<double> grid(resolution * resolution, 0.);
        const char* error;
        Py_BEGIN_ALLOW_THREADS
        error = rasterize(data, count, resolution, width, height, x, y, diameter/2, shape, angle, stock, grid);
        Py_END_ALLOW_THREADS
        if (error) { PyErr_SetString(PyExc_ValueError, error); return nullptr; }
        PyObject* rows = PyList_New(resolution);
        if (!rows) return nullptr;
        for (int j = 0; j < resolution; ++j) {
            PyObject* row = PyList_New(resolution);
            if (!row) { Py_DECREF(rows); return nullptr; }
            PyList_SET_ITEM(rows,j,row);
            for (int i = 0; i < resolution; ++i) {
                PyObject* v = PyFloat_FromDouble(grid[j*resolution+i]);
                if (!v) { Py_DECREF(rows); return nullptr; }
                PyList_SET_ITEM(row,i,v);
            }
        }
        return rows;
    } catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
}
