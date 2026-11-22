#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "kerfkite/gpu.hpp"
#include "kerfkite/parallel.hpp"

// Cutter contact for relief finishing: for each raster point, the highest a ball or flat cutter may stand without
// entering the conservative height field of cell maxima. The arithmetic is the reference's (studio/relief.py)
// statement for statement, so the depths are its depths to the last bit. Rasters are shared out between threads,
// each raster computed entirely by one thread, so the number of threads changes no bit either. Large jobs may run
// on a discrete GPU instead (kerfkite/gpu.hpp), with the same arithmetic in OpenCL C and a sample of every result
// checked bit for bit against the CPU; see try_gpu for when.

namespace {

// The largest map: 1023 x 1023 cells, from a 1024 x 1024 heightmap.
constexpr int kMaxCells = 1023;
// The most contact checks one job may make: about 4.5 s on one thread of the 24-core test machine (2.2 ns a
// check), a quarter of a second on all of its threads.
constexpr double kMaxChecks = 2000000000.0;
// A thread is started only for each two million checks, about 4 ms of work for one thread.
constexpr double kChecksPerThread = 2000000.0;
// Below this much work the GPU is not used: its fixed costs per job (buffers, transfers, launches, a few ms) would
// eat the gain. Measured on the test machine (24 threads, RTX 5090 Laptop GPU): 200 million checks take 24 ms on
// the threads and 11 ms on the GPU.
constexpr double kGpuChecks = 200000000.0;
// Setting a GPU up the first time in a process was measured at 90 to 290 ms. It is done once the large jobs of the
// process have spent this long on the threads, or when one job alone is expected to take this long.
constexpr double kSetupSeconds = 0.3;
// How many contacts of each GPU result are computed again on the CPU and compared.
constexpr std::size_t kGuardSamples = 128;

// The contact arithmetic of segment_contact below in OpenCL C, statement for statement, in double precision with
// no fused multiply-add and with std::max and std::min spelt out, so that it gives the CPU's bits. Each work item
// is one segment of one raster, numbered raster by raster.
const char *kReliefKernel = R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF
double most(double a, double b) { return a < b ? b : a; }
double least(double a, double b) { return b < a ? b : a; }
__kernel void relief_contact(__global const double *maxima, __global const double *xs, __global const double *ys,
                             const int rows, const int cols, const double width,
                             const double height, const double depth, const double diameter,
                             const double allowance, const int nx, const int ny, const int ball,
                             __global double *contacts, const long first, const long items) {
    const long item = first + (long)get_global_id(0);
    if (item >= items) return;
    const int segment = (int)(item % nx), raster = (int)(item / nx);
    const double dx = width / cols, dy = height / rows, radius = diameter / 2;
    const double py = height * raster / ny;
    const int lo_j = (int)most(0.0, floor((py - radius) / dy));
    const int hi_j = (int)least((double)(rows - 1), floor((py + radius) / dy));
    const double start = width * segment / nx, end = width * (segment + 1) / nx;
    const int lo_i = (int)most(0.0, floor((start - radius) / dx));
    const int hi_i = (int)least((double)(cols - 1), floor((end + radius) / dx));
    double best = -depth;
    for (int j = lo_j; j <= hi_j; j++) {
        const double yd = most(0.0, most(ys[j] - py, py - ys[j + 1]));
        for (int i = lo_i; i <= hi_i; i++) {
            const double xd = most(0.0, most(xs[i] - end, start - xs[i + 1]));
            const double ds = xd * xd + yd * yd;
            if (ds > radius * radius) continue;
            const double sag = ball ? radius - sqrt(most(0.0, radius * radius - ds)) : 0.0;
            best = most(best, maxima[j * cols + i] - sag);
        }
    }
    contacts[item] = least(0.0, best + allowance);
}
)CL";

// What the last job of the calling thread ran on.
thread_local std::string last_path = "none yet";
// The CPU's measured speed, in seconds a check on one thread, and the seconds the large jobs of this process have
// spent on the threads.
std::mutex speed_lock;
double cpu_seconds_per_check = 2.5e-9;
double cpu_large_seconds = 0;

// Whether to try the GPU for a job of this many checks that the CPU would run on `threads` threads. KERFKITE_GPU=on
// tries it for every job and KERFKITE_GPU=off for none. Otherwise only large jobs are considered, and the GPU is
// set up as a skier rents skis until the rent would have bought them: the CPU runs the large jobs until they have
// taken as long as setting the GPU up does, or until one job is expected to. From then on each job goes to
// whichever has been measured faster.
bool try_gpu(double checks, unsigned threads) {
    const std::string mode = kerfkite::gpu::mode();
    if (mode == "off") return false;
    if (mode == "on") return kerfkite::gpu::available();
    if (checks < kGpuChecks) return false;
    if (!kerfkite::gpu::started()) {
        std::lock_guard<std::mutex> guard(speed_lock);
        if (cpu_large_seconds + checks * cpu_seconds_per_check / threads < kSetupSeconds) return false;
    }
    if (!kerfkite::gpu::available()) return false;
    const double gpu = kerfkite::gpu::seconds_per_unit();
    if (gpu <= 0) return true;
    std::lock_guard<std::mutex> guard(speed_lock);
    return gpu < cpu_seconds_per_check / threads;
}

bool number(PyObject *o, double &value, double lo, double hi, const char *name) {
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

bool cell_value(double value, double depth) {
    if (!std::isfinite(value) || value < -depth || value > 0) {
        PyErr_SetString(PyExc_ValueError, "cell maximum is out of range"); return false;
    }
    return true;
}

struct Job {
    std::vector<double> maxima;  // rows x cols, row-major
    int rows = 0, cols = 0, nx = 0, ny = 0;
    double width = 0, height = 0, depth = 0, diameter = 0, allowance = 0;
    bool ball = false;
    // The cell edges i * dx (i = 0..cols) and j * dy (j = 0..rows), each the product the reference computes for
    // it, worked out once instead of at every check.
    std::vector<double> xs, ys;
};

// Fills the cell edges of a job whose settings are read.
void edges(Job &job) {
    const double dx=job.width/job.cols,dy=job.height/job.rows;
    job.xs.resize(std::size_t(job.cols)+1);
    job.ys.resize(std::size_t(job.rows)+1);
    for(int i=0;i<=job.cols;i++) job.xs[i]=i*dx;
    for(int j=0;j<=job.rows;j++) job.ys[j]=j*dy;
}

// The settings both entry points share, read and checked in the reference's order.
bool read_settings(PyObject *w, PyObject *h, PyObject *d, PyObject *diam, PyObject *allow, PyObject *xn,
                   PyObject *yn, PyObject *shape, Job &job) {
    double nx_d, ny_d;
    if (!number(w,job.width,.000001,10000,"width") || !number(h,job.height,.000001,10000,"height") ||
        !number(d,job.depth,.000001,1000,"depth") || !number(diam,job.diameter,.000001,1000,"diameter") ||
        !number(allow,job.allowance,0,job.depth,"allowance") || !number(xn,nx_d,1,99999,"nx") ||
        !number(yn,ny_d,1,99999,"ny")) return false;
    if (!PyLong_Check(xn) || !PyLong_Check(yn) || (nx_d+1)*(ny_d+1)>100000) {
        PyErr_SetString(PyExc_ValueError,"Integer raster dimensions must fit 100000 vertices");return false;
    }
    if (!PyUnicode_Check(shape)) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return false;}
    Py_ssize_t kind_size=0;
    const char *kind=PyUnicode_AsUTF8AndSize(shape,&kind_size);
    if (!kind) return false;
    if (kind_size!=4) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return false;}
    job.ball=strcmp(kind,"ball")==0;
    if (!job.ball && strcmp(kind,"flat")!=0) {PyErr_SetString(PyExc_ValueError,"shape must be flat or ball");return false;}
    job.nx=static_cast<int>(nx_d); job.ny=static_cast<int>(ny_d);
    return true;
}

bool too_much_work() {
    PyErr_SetString(PyExc_ValueError,"Native contact work exceeds 2000000000 checks");
    return false;
}

// A bound on the work from the map and raster sizes alone, before any value is read.
bool work_bound(const Job &job) {
    const double dx=job.width/job.cols,dy=job.height/job.rows;
    const double cx=std::min(double(job.cols),std::ceil((job.width/job.nx+job.diameter)/dx)+2);
    const double cy=std::min(double(job.rows),std::ceil(job.diameter/dy)+2);
    if (double(job.nx)*(double(job.ny)+1)*cx*cy>kMaxChecks) return too_much_work();
    return true;
}

// The checks the loops of contact_rows make, counted exactly with the same ranges.
double count_checks(const Job &job) {
    const double dx=job.width/job.cols,dy=job.height/job.rows,radius=job.diameter/2;
    double total=0;
    for(int raster=0;raster<=job.ny;raster++) {
        const double py=job.height*raster/job.ny;
        const int lo_j=static_cast<int>(std::max(0.0,std::floor((py-radius)/dy)));
        const int hi_j=static_cast<int>(std::min(double(job.rows-1),std::floor((py+radius)/dy)));
        if(hi_j<lo_j) continue;
        double across=0;
        for(int segment=0;segment<job.nx;segment++) {
            const double start=job.width*segment/job.nx,end=job.width*(segment+1)/job.nx;
            const int lo_i=static_cast<int>(std::max(0.0,std::floor((start-radius)/dx)));
            const int hi_i=static_cast<int>(std::min(double(job.cols-1),std::floor((end+radius)/dx)));
            if(hi_i>=lo_i) across+=hi_i-lo_i+1;
        }
        total+=across*(hi_j-lo_j+1);
    }
    return total;
}

// The deepest the cutter may go at one segment of one raster.
double segment_contact(const Job &job, int raster, int segment) {
    const int rows=job.rows,cols=job.cols,nx=job.nx,ny=job.ny;
    const double width=job.width,height=job.height;
    const double dx=width/cols,dy=height/rows,radius=job.diameter/2;
    const double *maxima=job.maxima.data(),*xs=job.xs.data(),*ys=job.ys.data();
    const double py=height*raster/ny;
    const int lo_j=static_cast<int>(std::max(0.0,std::floor((py-radius)/dy)));
    const int hi_j=static_cast<int>(std::min(double(rows-1),std::floor((py+radius)/dy)));
    const double start=width*segment/nx,end=width*(segment+1)/nx;
    const int lo_i=static_cast<int>(std::max(0.0,std::floor((start-radius)/dx)));
    const int hi_i=static_cast<int>(std::min(double(cols-1),std::floor((end+radius)/dx)));
    double best=-job.depth;
    for(int j=lo_j;j<=hi_j;j++) {
        const double yd=std::max(0.0,std::max(ys[j]-py,py-ys[j+1]));
        for(int i=lo_i;i<=hi_i;i++) {
            const double xd=std::max(0.0,std::max(xs[i]-end,start-xs[i+1]));
            const double ds=xd*xd+yd*yd;
            if(ds>radius*radius)continue;
            const double sag=job.ball?radius-std::sqrt(std::max(0.0,radius*radius-ds)):0;
            best=std::max(best,maxima[j*cols+i]-sag);
        }
    }
    return std::min(0.0,best+job.allowance);
}

// One raster's nx + 1 depths from its nx segment contacts: each vertex takes the deeper limit of its neighbours.
void vertex_row(const double *contacts, int nx, double *row) {
    for(int i=0;i<=nx;i++) row[i]=i==0?contacts[0]:i==nx?contacts[nx-1]:std::max(contacts[i-1],contacts[i]);
}

// Rasters first..last-1 of the answer: each raster is nx + 1 depths.
void contact_rows(const Job &job, std::size_t first, std::size_t last, double *output) {
    const int nx=job.nx;
    std::vector<double> contacts(nx);
    for(int raster=static_cast<int>(first);raster<static_cast<int>(last);raster++) {
        for(int segment=0;segment<nx;segment++) contacts[segment]=segment_contact(job,raster,segment);
        vertex_row(contacts.data(),nx,output+std::size_t(raster)*(nx+1));
    }
}

// The depths on the GPU, or false to compute them on the CPU. A spread sample of the GPU's contacts (the four
// corners and kGuardSamples - 4 others) is computed again on the CPU and compared bit for bit: on any difference
// the result is not used and the GPU is turned off for the rest of the process.
bool gpu_rows(const Job &job, double checks, std::vector<double> &output) {
    const int nx=job.nx,ny=job.ny;
    const std::size_t items=std::size_t(nx)*(ny+1);
    std::vector<double> contacts(items);
    const std::vector<kerfkite::gpu::Argument> arguments={
        kerfkite::gpu::Argument::in(job.maxima.data(),job.maxima.size()*sizeof(double)),
        kerfkite::gpu::Argument::in(job.xs.data(),job.xs.size()*sizeof(double)),
        kerfkite::gpu::Argument::in(job.ys.data(),job.ys.size()*sizeof(double)),
        kerfkite::gpu::Argument::whole(job.rows),kerfkite::gpu::Argument::whole(job.cols),
        kerfkite::gpu::Argument::number(job.width),kerfkite::gpu::Argument::number(job.height),
        kerfkite::gpu::Argument::number(job.depth),kerfkite::gpu::Argument::number(job.diameter),
        kerfkite::gpu::Argument::number(job.allowance),kerfkite::gpu::Argument::whole(nx),
        kerfkite::gpu::Argument::whole(ny),kerfkite::gpu::Argument::whole(job.ball?1:0),
        kerfkite::gpu::Argument::out(contacts.data(),contacts.size()*sizeof(double))};
    std::string why;
    double seconds=0;
    if(!kerfkite::gpu::run("relief_contact",kReliefKernel,arguments,items,why,seconds)) return false;
    std::vector<std::size_t> sample={0,std::size_t(nx-1),std::size_t(ny)*nx,items-1};
    for(std::size_t k=1;sample.size()<std::min(kGuardSamples,items);k++) sample.push_back(std::size_t(k*2654435761ull%items));
    for(std::size_t item:sample) {
        const double cpu=segment_contact(job,static_cast<int>(item/nx),static_cast<int>(item%nx));
        if(std::memcmp(&cpu,&contacts[item],sizeof cpu)!=0) {
            kerfkite::gpu::disable("its relief contact differed from the CPU's at raster "+std::to_string(item/nx)+
                                   ", segment "+std::to_string(item%nx));
            return false;
        }
    }
    kerfkite::gpu::record(checks,seconds);
    for(int raster=0;raster<=ny;raster++)
        vertex_row(&contacts[std::size_t(raster)*nx],nx,&output[std::size_t(raster)*(nx+1)]);
    last_path="GPU, "+kerfkite::gpu::describe();
    return true;
}

// Runs a job whose values are read, and returns its depths as (ny + 1) lists of nx + 1 floats.
PyObject *run(Job &job) {
    edges(job);
    const double checks=count_checks(job);
    if(checks>kMaxChecks) { too_much_work(); return nullptr; }
    std::vector<double> output(std::size_t(job.nx+1)*(job.ny+1));
    const std::size_t rasters=std::size_t(job.ny)+1;
    const unsigned threads=kerfkite::parallel::threads_for(checks,kChecksPerThread,rasters);
    bool failed=false, done=false;
    Py_BEGIN_ALLOW_THREADS
    try {
        if(try_gpu(checks,threads)) done=gpu_rows(job,checks,output);
    } catch(...) { done=false; }
    if(!done) {
        try {
            const auto began=std::chrono::steady_clock::now();
            kerfkite::parallel::for_ranges(rasters,threads,[&](std::size_t first,std::size_t last){contact_rows(job,first,last,output.data());});
            const double took=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
            last_path="CPU, "+std::to_string(threads)+(threads==1?" thread":" threads");
            std::lock_guard<std::mutex> guard(speed_lock);
            if(checks>=10000000) cpu_seconds_per_check=0.8*cpu_seconds_per_check+0.2*(took*threads/checks);
            if(checks>=kGpuChecks) cpu_large_seconds+=took;
        } catch(...) { failed=true; }
    }
    Py_END_ALLOW_THREADS
    if(failed) return PyErr_NoMemory();
    PyObject *result=PyList_New(job.ny+1);
    if(!result)return nullptr;
    for(int j=0;j<=job.ny;j++) {
        PyObject *row=PyList_New(job.nx+1);
        if(!row){Py_DECREF(result);return nullptr;}
        PyList_SET_ITEM(result,j,row);
        for(int i=0;i<=job.nx;i++) {
            PyObject *value=PyFloat_FromDouble(output[std::size_t(j)*(job.nx+1)+i]);
            if(!value){Py_DECREF(result);return nullptr;}
            PyList_SET_ITEM(row,i,value);
        }
    }
    return result;
}

} // namespace

// contact(maxima, width, height, depth, diameter, allowance, nx, ny, shape): maxima as a list of rows of floats.
static PyObject *contact(PyObject *, PyObject *args) {
    PyObject *matrix, *w, *h, *d, *diam, *allow, *xn, *yn, *shape;
    if (!PyArg_ParseTuple(args, "OOOOOOOOO", &matrix, &w, &h, &d, &diam, &allow, &xn, &yn, &shape)) return nullptr;
    Job job;
    if (!read_settings(w, h, d, diam, allow, xn, yn, shape, job)) return nullptr;
    if (!PyList_Check(matrix) || PyList_GET_SIZE(matrix)<1 || PyList_GET_SIZE(matrix)>kMaxCells) {
        PyErr_SetString(PyExc_ValueError,"maxima requires 1..1023 rows");return nullptr;
    }
    job.rows=static_cast<int>(PyList_GET_SIZE(matrix));
    PyObject *first=PyList_GET_ITEM(matrix,0);
    if (!PyList_Check(first) || PyList_GET_SIZE(first)<1 || PyList_GET_SIZE(first)>kMaxCells) {
        PyErr_SetString(PyExc_ValueError,"maxima requires 1..1023 columns");return nullptr;
    }
    job.cols=static_cast<int>(PyList_GET_SIZE(first));
    if (!work_bound(job)) return nullptr;
    try {
        job.maxima.resize(std::size_t(job.rows)*job.cols);
        for(int j=0;j<job.rows;j++) {
            PyObject *row=PyList_GET_ITEM(matrix,j);
            if(!PyList_Check(row)||PyList_GET_SIZE(row)!=job.cols) {PyErr_SetString(PyExc_ValueError,"maxima must be rectangular");return nullptr;}
            for(int i=0;i<job.cols;i++) if(!number(PyList_GET_ITEM(row,i),job.maxima[std::size_t(j)*job.cols+i],-job.depth,0,"cell maximum"))return nullptr;
        }
        return run(job);
    } catch(const std::bad_alloc &) { return PyErr_NoMemory(); }
}

// contact_packed(maxima, rows, columns, width, height, depth, diameter, allowance, nx, ny, shape): maxima as bytes
// holding rows x columns doubles in this machine's byte order, row by row. Large maps come this way so that a
// million values need not be built as Python objects.
static PyObject *contact_packed(PyObject *, PyObject *args) {
    PyObject *buffer, *w, *h, *d, *diam, *allow, *xn, *yn, *shape;
    int rows, cols;
    if (!PyArg_ParseTuple(args, "OiiOOOOOOOO", &buffer, &rows, &cols, &w, &h, &d, &diam, &allow, &xn, &yn, &shape)) return nullptr;
    Job job;
    if (!read_settings(w, h, d, diam, allow, xn, yn, shape, job)) return nullptr;
    if (rows<1 || rows>kMaxCells) {PyErr_SetString(PyExc_ValueError,"maxima requires 1..1023 rows");return nullptr;}
    if (cols<1 || cols>kMaxCells) {PyErr_SetString(PyExc_ValueError,"maxima requires 1..1023 columns");return nullptr;}
    if (!PyBytes_Check(buffer) || PyBytes_GET_SIZE(buffer)!=Py_ssize_t(rows)*cols*Py_ssize_t(sizeof(double))) {
        PyErr_SetString(PyExc_ValueError,"maxima must be bytes holding rows x columns doubles");return nullptr;
    }
    job.rows=rows; job.cols=cols;
    if (!work_bound(job)) return nullptr;
    try {
        job.maxima.resize(std::size_t(rows)*cols);
        std::memcpy(job.maxima.data(), PyBytes_AS_STRING(buffer), job.maxima.size()*sizeof(double));
        for(double value : job.maxima) if(!cell_value(value, job.depth)) return nullptr;
        return run(job);
    } catch(const std::bad_alloc &) { return PyErr_NoMemory(); }
}

// gpu_status(): the GPU in use, or why there is none (looking for it if that has not been done); last_contact():
// what the last contact job ran on.
static PyObject *gpu_status(PyObject *, PyObject *) {
    std::string text;
    Py_BEGIN_ALLOW_THREADS
    const bool off=kerfkite::gpu::mode()=="off";
    if(!off) kerfkite::gpu::available();
    text=kerfkite::gpu::describe();
    if(off&&!kerfkite::gpu::started()) text="turned off by KERFKITE_GPU=off";
    Py_END_ALLOW_THREADS
    return PyUnicode_FromString(text.c_str());
}

static PyObject *last_contact(PyObject *, PyObject *) {
    return PyUnicode_FromString(last_path.c_str());
}

PyObject* stock_heights(PyObject*, PyObject*);
PyObject* fixed_text(PyObject*, PyObject*);
PyObject* rounded_value(PyObject*, PyObject*);
PyObject* segment_rows(PyObject*, PyObject*);
PyObject* core_call(PyObject*, PyObject*);
PyObject* core_cam(PyObject*, PyObject*);
PyObject* core_cam_text(PyObject*, PyObject*);
PyObject* solid_call(PyObject*, PyObject*);
PyObject* solid_mesh(PyObject*, PyObject*);

static PyMethodDef methods[]={{"stock_heights",stock_heights,METH_VARARGS,"Compute a bounded stock-removal grid from planar moves."},
    {"fixed_text",fixed_text,METH_VARARGS,"Format a float with a fixed number of decimals exactly as Python does."},
    {"rounded_value",rounded_value,METH_VARARGS,"Round a float to 0..3 decimals exactly as Python does."},
    {"segment_rows",segment_rows,METH_VARARGS,"Unpack a packed segment buffer into list rows."},
    {"core_call",core_call,METH_VARARGS,"Run a command of the native core: (command, JSON request) -> JSON answer."},
    {"core_cam",core_cam,METH_VARARGS,"G-code or rotary output from the native core: (command, request, packed=False) -> job, or None when the request cannot cross unchanged."},
    {"core_cam_text",core_cam_text,METH_VARARGS,"The answer of core_cam as UTF-8 JSON text written by the core: (command, request) -> bytes, or None."},
    {"solid_call",solid_call,METH_VARARGS,"Run a solid request of the native core with solids kept from earlier calls: (JSON request, kept) -> (JSON answer, solid)."},
    {"solid_mesh",solid_mesh,METH_VARARGS,"The mesh of a kept solid as bytes: (solid) -> (float64 vertices, uint32 triangles)."},
    {"contact",contact,METH_VARARGS,"Compute bounded conservative cutter contact depths from cell maxima."},
    {"contact_packed",contact_packed,METH_VARARGS,"Cutter contact depths from cell maxima packed as bytes of doubles: (maxima, rows, columns, ...)."},
    {"gpu_status",gpu_status,METH_NOARGS,"The GPU in use for relief contact, or why there is none."},
    {"last_contact",last_contact,METH_NOARGS,"What the calling thread's last relief contact job ran on: 'CPU, n threads' or 'GPU, device'."},
    {nullptr,nullptr,0,nullptr}};
static PyModuleDef module={PyModuleDef_HEAD_INIT,"_relief_native","Optional CNC Studio relief contact kernel.",-1,methods};
PyMODINIT_FUNC PyInit__relief_native(void) {return PyModule_Create(&module);}
