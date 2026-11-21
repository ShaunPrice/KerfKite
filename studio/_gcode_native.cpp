// Optional C++ loops for G-code and rotary generation.
//
// Python is the reference implementation and owns validation, geometry, program structure and every error
// message. These functions cover only the per-move loops, and only the plain case: when anything is unusual
// (an unexpected type, a non-finite number, a limit or fixture violation) they return None and the caller
// runs the Python loop from the same state, so behaviour outside the plain case is the reference's by
// construction. They hold the GIL throughout.
//
// The text must match Python's to the byte. Fixed-point formatting is therefore done with exact integer
// arithmetic (round half to even on the exact binary value, as Python formats floats) instead of the C
// library, whose rounding has differed between platforms. Squares go through the same libm pow call that
// Python's ** operator makes, and the rotary cut length is computed by the Python function itself.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "kerfkite/number_text.hpp"

namespace {

// Exact fixed-point text, round() and squares, shared with the Python-free core.
using kerfkite::text::fixed;
using kerfkite::text::rounded;
using kerfkite::text::square;


// A plain float or int (not bool, not a subclass), as a finite double.
bool number(PyObject *object, double &value) {
    if (PyFloat_CheckExact(object)) {
        value = PyFloat_AS_DOUBLE(object);
    } else if (PyLong_CheckExact(object)) {
        value = PyLong_AsDouble(object);
        if (value == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
    } else {
        return false;
    }
    return std::isfinite(value);
}

// The items of an exact list or tuple of the given length.
PyObject **items(PyObject *sequence, Py_ssize_t length) {
    if (PyList_CheckExact(sequence)) {
        return PyList_GET_SIZE(sequence) == length ? PySequence_Fast_ITEMS(sequence) : nullptr;
    }
    if (PyTuple_CheckExact(sequence)) {
        return PyTuple_GET_SIZE(sequence) == length ? PySequence_Fast_ITEMS(sequence) : nullptr;
    }
    return nullptr;
}


struct Lines {
    std::string text;
    Py_ssize_t count = 0;
    std::string &begin() {
        if (count++) text.push_back('\n');
        return text;
    }
};

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

// gcode_moves(paths, depths, levels, laser, rotary, packed, safe, plunge, cut_feed)
//   -> (text, segments, length, runs) or None
// text holds the loop's lines joined by newlines. segments is a bytes buffer of native-order rows when packed,
// otherwise a list of rows holding the caller's own number objects. In rotary mode each path contributes an
// empty line whose index, with the path's first and last row, is listed in runs.
PyObject *gcode_moves(PyObject *, PyObject *args) {
    PyObject *paths, *depths, *levels;
    int laser, rotary, packed;
    double safe, plunge, cut_feed;
    if (!PyArg_ParseTuple(args, "OOOpppddd", &paths, &depths, &levels, &laser, &rotary, &packed, &safe, &plunge,
                          &cut_feed))
        return nullptr;
    const bool has_depth = depths != Py_None;
    if (!PyList_CheckExact(paths) || !PyList_CheckExact(levels) || (has_depth && laser)) Py_RETURN_NONE;
    const Py_ssize_t path_count = PyList_GET_SIZE(paths), level_count = PyList_GET_SIZE(levels);
    if (!path_count || !level_count) Py_RETURN_NONE;
    if (has_depth && (!PyList_CheckExact(depths) || PyList_GET_SIZE(depths) != path_count)) Py_RETURN_NONE;
    const int width = laser ? 3 : 2;
    try {
        Lines lines;
        std::string rows, safe_line = "G0 Z", plunge_word = " F", feed_word = " F";
        if (!fixed(safe_line, safe, 3) || !fixed(plunge_word, plunge, 1) || !fixed(feed_word, cut_feed, 1))
            Py_RETURN_NONE;
        Reference list(packed ? nullptr : PyList_New(0)), runs(PyList_New(0)), zero(PyLong_FromLong(0));
        if ((!packed && !list.object) || !runs.object || !zero.object) return nullptr;
        double length = 0;
        Py_ssize_t count = 0;
        for (Py_ssize_t level = 0; level < level_count; level++) {
            PyObject *z_object = PyList_GET_ITEM(levels, level);
            double z = 0;
            // A laser pass has no Z level; a router level is a float.
            if (laser ? z_object != Py_None : !(PyFloat_CheckExact(z_object) && number(z_object, z))) Py_RETURN_NONE;
            for (Py_ssize_t index = 0; index < path_count; index++) {
                PyObject *path = PyList_GET_ITEM(paths, index);
                if (!PyList_CheckExact(path) || PyList_GET_SIZE(path) < 2) Py_RETURN_NONE;
                const Py_ssize_t points = PyList_GET_SIZE(path);
                PyObject *row_depths = has_depth ? PyList_GET_ITEM(depths, index) : nullptr;
                if (has_depth && (!PyList_CheckExact(row_depths) || PyList_GET_SIZE(row_depths) != points))
                    Py_RETURN_NONE;
                PyObject **first = items(PyList_GET_ITEM(path, 0), width);
                double x, y, power = 0;
                if (!first || !number(first[0], x) || !number(first[1], y)) Py_RETURN_NONE;
                PyObject *x_object = first[0], *y_object = first[1];
                // Python's max(z, depth) keeps z unless the depth is greater.
                PyObject *current_object = laser ? zero.object : z_object;
                double current = z;
                if (has_depth) {
                    PyObject *depth = PyList_GET_ITEM(row_depths, 0);
                    double value;
                    if (!PyFloat_CheckExact(depth) || !number(depth, value)) Py_RETURN_NONE;
                    if (value > z) {
                        current = value;
                        current_object = depth;
                    }
                }
                if (laser) {
                    if (!number(first[2], power)) Py_RETURN_NONE;
                    lines.begin() += "M5";
                    std::string &move = lines.begin();
                    move += "G0 X";
                    if (!fixed(move, x, 3)) Py_RETURN_NONE;
                    move += " Y";
                    if (!fixed(move, y, 3)) Py_RETURN_NONE;
                    std::string &on = lines.begin();
                    on += "M4 S";
                    if (!fixed(on, power, 0)) Py_RETURN_NONE;
                } else {
                    lines.begin() += safe_line;
                    std::string &move = lines.begin();
                    move += "G0 X";
                    if (!fixed(move, x, 3)) Py_RETURN_NONE;
                    move += " Y";
                    if (!fixed(move, y, 3)) Py_RETURN_NONE;
                    std::string &down = lines.begin();
                    down += "G1 Z";
                    if (!fixed(down, current, 3)) Py_RETURN_NONE;
                    down += plunge_word;
                }
                const Py_ssize_t run_start = count;
                double start = laser ? 0 : current;
                PyObject *start_object = current_object;
                for (Py_ssize_t vertex = 1; vertex < points; vertex++) {
                    PyObject **point = items(PyList_GET_ITEM(path, vertex), width);
                    double nx, ny;
                    if (!point || !number(point[0], nx) || !number(point[1], ny)) Py_RETURN_NONE;
                    if (laser && !number(point[2], power)) Py_RETURN_NONE;
                    double end = start;
                    PyObject *end_object = start_object;
                    if (has_depth) {
                        PyObject *depth = PyList_GET_ITEM(row_depths, vertex);
                        double value;
                        if (!PyFloat_CheckExact(depth) || !number(depth, value)) Py_RETURN_NONE;
                        if (value > z) {
                            end = value;
                            end_object = depth;
                        } else {
                            end = z;
                            end_object = z_object;
                        }
                    }
                    if (!rotary) {
                        std::string &cut = lines.begin();
                        cut += "G1 X";
                        if (!fixed(cut, nx, 3)) Py_RETURN_NONE;
                        cut += " Y";
                        if (!fixed(cut, ny, 3)) Py_RETURN_NONE;
                        cut += feed_word;
                        if (laser) {
                            cut += " S";
                            if (!fixed(cut, power, 0)) Py_RETURN_NONE;
                        } else if (has_depth) {
                            cut += " Z";
                            if (!fixed(cut, end, 3)) Py_RETURN_NONE;
                        }
                    }
                    if (packed) {
                        const double row[7] = {x, y, nx, ny, end, laser ? power : std::nan(""), start};
                        rows.append(reinterpret_cast<const char *>(row), sizeof row);
                    } else {
                        PyObject *row = PyList_New(7);
                        if (!row) return nullptr;
                        PyObject *fields[7] = {x_object, y_object, point[0], point[1],
                                               end_object, laser ? point[2] : Py_None, start_object};
                        for (int field = 0; field < 7; field++) {
                            Py_INCREF(fields[field]);
                            PyList_SET_ITEM(row, field, fields[field]);
                        }
                        const int failed = PyList_Append(list.object, row);
                        Py_DECREF(row);
                        if (failed) return nullptr;
                    }
                    length += std::sqrt(square(nx - x) + square(ny - y) + square(end - start));
                    count++;
                    start = end;
                    start_object = end_object;
                    x = nx;
                    y = ny;
                    x_object = point[0];
                    y_object = point[1];
                }
                if (rotary) {
                    Reference run(Py_BuildValue("(nnn)", lines.count, run_start, count));
                    lines.begin();
                    if (!run.object || PyList_Append(runs.object, run.object)) return nullptr;
                }
                lines.begin() += laser ? "M5" : safe_line;
            }
        }
        if (!count) Py_RETURN_NONE;
        Reference text(PyUnicode_FromStringAndSize(lines.text.data(), static_cast<Py_ssize_t>(lines.text.size())));
        Reference segments(packed ? PyBytes_FromStringAndSize(rows.data(), static_cast<Py_ssize_t>(rows.size()))
                                  : list.release());
        if (!text.object || !segments.object) return nullptr;
        return Py_BuildValue("(NNdN)", text.release(), segments.release(), length, runs.release());
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    }
}

// rotary_run(rows, start, stop, feed, variable_z, laser, wrapped_x, circumference, offset, direction, radius,
//            fixture, axial_min, axial_max, min_radius, letters, x, y, z, inverse_active, emitting, minutes,
//            min_z, max_z, min_axial, max_axial, min_angle, max_angle, cut_length, radius_object, reference)
//   -> (text, x, y, z, inverse_active, minutes, min_z, max_z, min_axial, max_axial, min_angle, max_angle)
//      or None
// Translates one generated cutting run. Nothing is written back: the caller applies the returned state.
PyObject *rotary_run(PyObject *, PyObject *args) {
    Py_buffer rows;
    Py_ssize_t begin, end;
    double feed, circumference, offset, direction, radius, axial_min, axial_max, min_radius, minutes;
    double min_z, max_z, min_axial, max_axial, min_angle, max_angle;
    int variable_z, laser, wrapped_x, fixture, inverse_active, emitting;
    const char *letters;
    Py_ssize_t letter_count;
    PyObject *x_object, *y_object, *z_object, *cut_length, *radius_object, *reference;
    if (!PyArg_ParseTuple(args, "y*nndpppddddpddds#OOOppdddddddOOO", &rows, &begin, &end, &feed, &variable_z, &laser,
                          &wrapped_x, &circumference, &offset, &direction, &radius, &fixture, &axial_min, &axial_max,
                          &min_radius, &letters, &letter_count, &x_object, &y_object, &z_object, &inverse_active,
                          &emitting, &minutes, &min_z, &max_z, &min_axial, &max_axial, &min_angle, &max_angle,
                          &cut_length, &radius_object, &reference))
        return nullptr;
    struct Release {
        Py_buffer *view;
        ~Release() { PyBuffer_Release(view); }
    } release = {&rows};
    const Py_ssize_t size = 7 * static_cast<Py_ssize_t>(sizeof(double));
    if (letter_count != 3 || begin < 0 || end < begin || rows.len % size || end > rows.len / size) Py_RETURN_NONE;
    if ((laser && !emitting) || !(feed >= 0.001 && feed <= 100000) || !(circumference > 0)) Py_RETURN_NONE;
    double x, y, z = 0;
    const bool z_known = z_object != Py_None;
    if (!number(x_object, x) || !number(y_object, y) || (z_known && !number(z_object, z))) Py_RETURN_NONE;
    if (variable_z && !z_known) Py_RETURN_NONE;
    const char drive = letters[0], along = letters[1], radial = letters[2];
    try {
        Lines lines;
        const char *data = static_cast<const char *>(rows.buf);
        for (Py_ssize_t index = begin; index < end; index++) {
            double row[7];
            std::memcpy(row, data + index * size, sizeof row);
            double nx, ny, nz = z, power = 0;
            if (!rounded(row[2], 3, nx) || !rounded(row[3], 3, ny)) Py_RETURN_NONE;
            if (variable_z && !rounded(row[4], 3, nz)) Py_RETURN_NONE;
            if (laser && (!rounded(row[5], 0, power) || !(power >= 0 && power <= 100000))) Py_RETURN_NONE;
            if (variable_z || z_known) {
                if (radius + nz <= 0) Py_RETURN_NONE;
                if (fixture && radius + nz < min_radius - 1e-9) Py_RETURN_NONE;
                if (nz < min_z) min_z = nz;
                if (nz > max_z) max_z = nz;
            }
            const double axial = wrapped_x ? ny : nx, wrapped = wrapped_x ? nx : ny;
            if (fixture && !(axial_min - 1e-9 <= axial && axial <= axial_max + 1e-9)) Py_RETURN_NONE;
            if (axial < min_axial) min_axial = axial;
            if (axial > max_axial) max_axial = axial;
            // The cut length belongs to the Python reference: it uses Python's own hypot and asinh.
            Reference arc(PyFloat_FromDouble(wrapped - (wrapped_x ? x : y)));
            Reference travel(PyFloat_FromDouble(axial - (wrapped_x ? y : x)));
            Reference depth(PyFloat_FromDouble(variable_z ? nz - z : 0.0));
            Reference from(PyFloat_FromDouble(z_known && z != 0 ? z : 0.0));
            if (!arc.object || !travel.object || !depth.object || !from.object) return nullptr;
            Reference measured(PyObject_CallFunctionObjArgs(cut_length, arc.object, travel.object, depth.object,
                                                            radius_object, from.object, reference, nullptr));
            double length;
            if (!measured.object || !number(measured.object, length)) {
                PyErr_Clear();
                Py_RETURN_NONE;
            }
            // Seam snap and angle, in the reference's order of operations.
            const double turn = std::nearbyint(wrapped / circumference);
            const double arc_value = std::fabs(wrapped - turn * circumference) <= 0.00051 ? turn * circumference : wrapped;
            const double angle = offset + direction * 360 * arc_value / circumference;
            if (std::fabs(angle) > 360000) Py_RETURN_NONE;
            if (angle < min_angle) min_angle = angle;
            if (angle > max_angle) max_angle = angle;
            x = nx;
            y = ny;
            if (variable_z) z = nz;
            if (length <= 1e-12) continue;
            const double inverse = feed / length;
            if (!(inverse >= 1e-9 && inverse <= 1e9)) Py_RETURN_NONE;
            if (!inverse_active) {
                lines.begin() += "G93";
                inverse_active = 1;
            }
            std::string &block = lines.begin();
            block += "G1 ";
            for (int key = 0; key < 2; key++) {  // X then Y, as the source words are ordered
                const bool is_wrapped = (key == 0) == (wrapped_x != 0);
                block.push_back(is_wrapped ? drive : along);
                if (!fixed(block, is_wrapped ? angle : axial, 6)) Py_RETURN_NONE;
                block.push_back(' ');
            }
            if (variable_z) {
                block.push_back(radial);
                if (!fixed(block, nz, 6)) Py_RETURN_NONE;
                block.push_back(' ');
            }
            block.push_back('F');
            if (!fixed(block, inverse, 9)) Py_RETURN_NONE;
            if (laser) {
                block += " S";
                if (!fixed(block, power, 0)) Py_RETURN_NONE;
            }
            minutes += length / feed;
        }
        return Py_BuildValue("(s#ddNOddddddd)", lines.text.data(), static_cast<Py_ssize_t>(lines.text.size()), x, y,
                             variable_z || z_known ? PyFloat_FromDouble(z) : (Py_INCREF(Py_None), Py_None),
                             inverse_active ? Py_True : Py_False, minutes, min_z, max_z, min_axial, max_axial,
                             min_angle, max_angle);
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    }
}

// plain_points(paths, width, height, limit) -> list of paths of [float, float], or None
// The plain case of input validation: every path is a list of at least two [x, y] lists holding exact floats
// or ints within the work area, and there are at most limit points. Anything else returns None so that the
// Python validator runs and reports it.
PyObject *plain_points(PyObject *, PyObject *args) {
    PyObject *paths;
    double width, height;
    Py_ssize_t limit;
    if (!PyArg_ParseTuple(args, "Oddn", &paths, &width, &height, &limit)) return nullptr;
    if (!PyList_CheckExact(paths)) Py_RETURN_NONE;
    const Py_ssize_t path_count = PyList_GET_SIZE(paths);
    Reference clean(PyList_New(path_count));
    if (!clean.object) return nullptr;
    Py_ssize_t total = 0;
    for (Py_ssize_t index = 0; index < path_count; index++) {
        PyObject *path = PyList_GET_ITEM(paths, index);
        if (!PyList_CheckExact(path) || PyList_GET_SIZE(path) < 2) Py_RETURN_NONE;
        const Py_ssize_t points = PyList_GET_SIZE(path);
        total += points;
        if (total > limit) Py_RETURN_NONE;
        PyObject *copy = PyList_New(points);
        if (!copy) return nullptr;
        PyList_SET_ITEM(clean.object, index, copy);
        for (Py_ssize_t vertex = 0; vertex < points; vertex++) {
            PyObject *point = PyList_GET_ITEM(path, vertex);
            if (!PyList_CheckExact(point) || PyList_GET_SIZE(point) != 2) Py_RETURN_NONE;
            PyObject *pair = PyList_New(2);
            if (!pair) return nullptr;
            PyList_SET_ITEM(copy, vertex, pair);
            for (int axis = 0; axis < 2; axis++) {
                PyObject *value = PyList_GET_ITEM(point, axis);
                double number_value;
                if (!number(value, number_value) || !(number_value >= 0 && number_value <= (axis ? height : width)))
                    Py_RETURN_NONE;
                // float(x) is x itself for an exact float; an int becomes a new float.
                PyObject *converted = PyFloat_CheckExact(value) ? (Py_INCREF(value), value) : PyFloat_FromDouble(number_value);
                if (!converted) return nullptr;
                PyList_SET_ITEM(pair, axis, converted);
            }
        }
    }
    return clean.release();
}

// plain_depths(depths, paths) -> list of lists of float, or None
// The plain case of per-vertex depth validation: one list per path, one exact float or int in -50..0 per vertex.
PyObject *plain_depths(PyObject *, PyObject *args) {
    PyObject *depths, *paths;
    if (!PyArg_ParseTuple(args, "OO", &depths, &paths)) return nullptr;
    if (!PyList_CheckExact(depths) || !PyList_CheckExact(paths)) Py_RETURN_NONE;
    const Py_ssize_t path_count = PyList_GET_SIZE(paths);
    if (PyList_GET_SIZE(depths) != path_count) Py_RETURN_NONE;
    Reference clean(PyList_New(path_count));
    if (!clean.object) return nullptr;
    for (Py_ssize_t index = 0; index < path_count; index++) {
        PyObject *values = PyList_GET_ITEM(depths, index), *path = PyList_GET_ITEM(paths, index);
        if (!PyList_CheckExact(values) || !PyList_CheckExact(path)) Py_RETURN_NONE;
        const Py_ssize_t points = PyList_GET_SIZE(path);
        if (PyList_GET_SIZE(values) != points) Py_RETURN_NONE;
        PyObject *copy = PyList_New(points);
        if (!copy) return nullptr;
        PyList_SET_ITEM(clean.object, index, copy);
        for (Py_ssize_t vertex = 0; vertex < points; vertex++) {
            PyObject *value = PyList_GET_ITEM(values, vertex);
            double depth;
            if (!number(value, depth) || !(depth >= -50 && depth <= 0)) Py_RETURN_NONE;
            PyObject *converted = PyFloat_CheckExact(value) ? (Py_INCREF(value), value) : PyFloat_FromDouble(depth);
            if (!converted) return nullptr;
            PyList_SET_ITEM(copy, vertex, converted);
        }
    }
    return clean.release();
}

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

// rounded_value(value, decimals) -> float or None. Python's round(value, decimals) for 0..3 decimals.
PyObject *rounded_value(PyObject *, PyObject *args) {
    double value, result;
    int decimals;
    if (!PyArg_ParseTuple(args, "di", &value, &decimals)) return nullptr;
    if (decimals < 0 || decimals > 3 || !rounded(value, decimals, result)) Py_RETURN_NONE;
    return PyFloat_FromDouble(result);
}
