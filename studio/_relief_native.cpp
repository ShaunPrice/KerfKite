#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

static bool number(PyObject *o, double &value, double lo, double hi, const char *name) {
    if (PyBool_Check(o) || !(PyFloat_Check(o) || PyLong_Check(o))) {
        PyErr_Format(PyExc_ValueError, "%s must be numeric", name); return false;
    }
    value = PyFloat_AsDouble(o);
    if (PyErr_Occurred()) { PyErr_Clear(); PyErr_Format(PyExc_ValueError, "%s is out of range", name); return false; }
    if (!std::isfinite(value) || value < lo || value > hi) {
        PyErr_Format(PyExc_ValueError, "%s is out of range", name); return false;
    }
    return true;
}

static PyObject *contact(PyObject *, PyObject *args) {
    PyObject *matrix, *w, *h, *d, *diam, *allow, *xn, *yn, *shape;
    if (!PyArg_ParseTuple(args, "OOOOOOOOO", &matrix, &w, &h, &d, &diam, &allow, &xn, &yn, &shape)) return nullptr;
    double width, height, depth, diameter, allowance, nx_d, ny_d;
    if (!number(w,width,.000001,10000,"width") || !number(h,height,.000001,10000,"height") ||
        !number(d,depth,.000001,1000,"depth") || !number(diam,diameter,.000001,1000,"diameter") ||
        !number(allow,allowance,0,depth,"allowance") || !number(xn,nx_d,1,99999,"nx") ||
        !number(yn,ny_d,1,99999,"ny")) return nullptr;
    if (!PyLong_Check(xn) || !PyLong_Check(yn) || (nx_d+1)*(ny_d+1)>100000) {
        PyErr_SetString(PyExc_ValueError,"Integer raster dimensions must fit 100000 vertices");return nullptr;
    }
    if (!PyUnicode_Check(shape)) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return nullptr;}
    Py_ssize_t kind_size=0;
    const char *kind=PyUnicode_AsUTF8AndSize(shape,&kind_size);
    if (!kind) return nullptr;
    if (kind_size!=4) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return nullptr;}
    const bool ball=strcmp(kind,"ball")==0;
    if (!ball && strcmp(kind,"flat")!=0) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return nullptr;}
    if (!PyList_Check(matrix) || PyList_GET_SIZE(matrix)<1 || PyList_GET_SIZE(matrix)>255) {
        PyErr_SetString(PyExc_ValueError,"maxima requires 1..255 rows");return nullptr;
    }
    const int rows=static_cast<int>(PyList_GET_SIZE(matrix));
    PyObject *first=PyList_GET_ITEM(matrix,0);
    if (!PyList_Check(first) || PyList_GET_SIZE(first)<1 || PyList_GET_SIZE(first)>255) {
        PyErr_SetString(PyExc_ValueError,"maxima requires 1..255 columns");return nullptr;
    }
    const int cols=static_cast<int>(PyList_GET_SIZE(first)), nx=static_cast<int>(nx_d),ny=static_cast<int>(ny_d);
    const double dx=width/cols,dy=height/rows,radius=diameter/2;
    const double cx=std::min(double(cols),std::ceil((width/nx+diameter)/dx)+2);
    const double cy=std::min(double(rows),std::ceil(diameter/dy)+2);
    if (nx_d*(ny_d+1)*cx*cy>10000000) {
        PyErr_SetString(PyExc_ValueError,"Native contact work exceeds 10000000 checks");return nullptr;
    }
    try {
        std::vector<double> maxima(rows*cols), output((nx+1)*(ny+1)), contacts(nx);
        for(int j=0;j<rows;j++) {
            PyObject *row=PyList_GET_ITEM(matrix,j);
            if(!PyList_Check(row)||PyList_GET_SIZE(row)!=cols) {PyErr_SetString(PyExc_ValueError,"maxima must be rectangular");return nullptr;}
            for(int i=0;i<cols;i++) if(!number(PyList_GET_ITEM(row,i),maxima[j*cols+i],-depth,0,"cell maximum"))return nullptr;
        }
        bool overflow=false;long checks=0;
        Py_BEGIN_ALLOW_THREADS
        for(int raster=0;raster<=ny && !overflow;raster++) {
            const double py=height*raster/ny;
            const int lo_j=static_cast<int>(std::max(0.0,std::floor((py-radius)/dy)));
            const int hi_j=static_cast<int>(std::min(double(rows-1),std::floor((py+radius)/dy)));
            for(int segment=0;segment<nx;segment++) {
                const double start=width*segment/nx,end=width*(segment+1)/nx;
                const int lo_i=static_cast<int>(std::max(0.0,std::floor((start-radius)/dx)));
                const int hi_i=static_cast<int>(std::min(double(cols-1),std::floor((end+radius)/dx)));
                double best=-depth;
                for(int j=lo_j;j<=hi_j;j++) {
                    const double yd=std::max(0.0,std::max(j*dy-py,py-(j+1)*dy));
                    for(int i=lo_i;i<=hi_i;i++) {
                        if(++checks>10000000) {overflow=true;break;}
                        const double xd=std::max(0.0,std::max(i*dx-end,start-(i+1)*dx));
                        const double ds=xd*xd+yd*yd;
                        if(ds>radius*radius)continue;
                        const double sag=ball?radius-std::sqrt(std::max(0.0,radius*radius-ds)):0;
                        best=std::max(best,maxima[j*cols+i]-sag);
                    }
                    if(overflow)break;
                }
                if(overflow)break;
                contacts[segment]=std::min(0.0,best+allowance);
            }
            if(overflow)break;
            for(int i=0;i<=nx;i++) output[raster*(nx+1)+i]=i==0?contacts[0]:i==nx?contacts[nx-1]:std::max(contacts[i-1],contacts[i]);
        }
        Py_END_ALLOW_THREADS
        if(overflow) {PyErr_SetString(PyExc_ValueError,"Native contact work exceeds 10000000 checks");return nullptr;}
        PyObject *result=PyList_New(ny+1);
        if(!result)return nullptr;
        for(int j=0;j<=ny;j++) {
            PyObject *row=PyList_New(nx+1);
            if(!row){Py_DECREF(result);return nullptr;}
            PyList_SET_ITEM(result,j,row);
            for(int i=0;i<=nx;i++) {
                PyObject *value=PyFloat_FromDouble(output[j*(nx+1)+i]);
                if(!value){Py_DECREF(result);return nullptr;}
                PyList_SET_ITEM(row,i,value);
            }
        }
        return result;
    } catch(const std::bad_alloc &) { return PyErr_NoMemory(); }
}

PyObject* stock_heights(PyObject*, PyObject*);

static PyMethodDef methods[]={{"stock_heights",stock_heights,METH_VARARGS,"Compute a bounded stock-removal grid from planar moves."},{"contact",contact,METH_VARARGS,"Compute bounded conservative cutter contact depths from cell maxima."},{nullptr,nullptr,0,nullptr}};
static PyModuleDef module={PyModuleDef_HEAD_INIT,"_relief_native","Optional CNC Studio relief contact kernel.",-1,methods};
PyMODINIT_FUNC PyInit__relief_native(void) {return PyModule_Create(&module);}
