#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "ScriptletEngine.h"
#include <string>

// ---------------------------------------------------------------------------
// Static callbacks — safe because only one ScriptletEngine exists per process.
static std::function<void()>                   s_goCb;
static std::function<void(const std::string&)> s_selectCb;
static std::function<void(const std::string&)> s_alertCb;
static std::function<void(const std::string&)> s_outputCb;

// ---------------------------------------------------------------------------
// mcp Python module methods

static PyObject* py_go(PyObject*, PyObject*) {
    if (s_goCb) s_goCb();
    Py_RETURN_NONE;
}

static PyObject* py_select(PyObject*, PyObject* args) {
    const char* num = nullptr;
    if (!PyArg_ParseTuple(args, "s", &num)) return nullptr;
    if (s_selectCb && num) s_selectCb(num);
    Py_RETURN_NONE;
}

static PyObject* py_alert(PyObject*, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    if (s_alertCb && msg) s_alertCb(msg);
    Py_RETURN_NONE;
}

static PyMethodDef kMcpMethods[] = {
    {"go",     py_go,     METH_NOARGS,  "Trigger Go (advance and fire selected cue)"},
    {"select", py_select, METH_VARARGS, "select(cueNum: str) — set the selected cue by number"},
    {"alert",  py_alert,  METH_VARARGS, "alert(msg: str) — show an alert dialog"},
    {nullptr, nullptr, 0, nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kMcpModule = {
    PyModuleDef_HEAD_INIT, "mcp", "Music Cue Player scripting API", -1, kMcpMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp() {
    return PyModule_Create(&kMcpModule);
}

// ---------------------------------------------------------------------------
// ScriptletEngine

ScriptletEngine::ScriptletEngine() {
    PyImport_AppendInittab("mcp", &PyInit_mcp);
    Py_Initialize();
    m_initialized = true;
}

ScriptletEngine::~ScriptletEngine() {
    if (m_initialized) Py_FinalizeEx();
}

void ScriptletEngine::setGoCallback    (std::function<void()> cb)                   { s_goCb     = std::move(cb); }
void ScriptletEngine::setSelectCallback(std::function<void(const std::string&)> cb) { s_selectCb = std::move(cb); }
void ScriptletEngine::setAlertCallback (std::function<void(const std::string&)> cb) { s_alertCb  = std::move(cb); }
void ScriptletEngine::setOutputCallback(std::function<void(const std::string&)> cb) { s_outputCb = std::move(cb); }

std::string ScriptletEngine::run(const std::string& code) {
    if (code.empty()) return {};

    // Redirect sys.stdout and sys.stderr to StringIO for capture.
    PyObject* sys       = PyImport_ImportModule("sys");
    PyObject* ioMod     = PyImport_ImportModule("io");
    PyObject* capOut    = PyObject_CallMethod(ioMod, "StringIO", nullptr);
    PyObject* capErr    = PyObject_CallMethod(ioMod, "StringIO", nullptr);
    PyObject* oldOut    = PyObject_GetAttrString(sys, "stdout");
    PyObject* oldErr    = PyObject_GetAttrString(sys, "stderr");
    PyObject_SetAttrString(sys, "stdout", capOut);
    PyObject_SetAttrString(sys, "stderr", capErr);

    // Run in an isolated namespace (fresh dict with builtins injected).
    PyObject* builtins = PyImport_ImportModule("builtins");
    PyObject* globals  = PyDict_New();
    PyDict_SetItemString(globals, "__builtins__", builtins);
    Py_DECREF(builtins);

    PyObject* result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
    Py_DECREF(globals);

    // Restore stdout/stderr and extract captured output.
    PyObject_SetAttrString(sys, "stdout", oldOut);
    PyObject_SetAttrString(sys, "stderr", oldErr);
    Py_DECREF(oldOut); Py_DECREF(oldErr);

    auto extractStr = [](PyObject* sio) -> std::string {
        PyObject* v = PyObject_CallMethod(sio, "getvalue", nullptr);
        std::string s;
        if (v) { if (const char* cs = PyUnicode_AsUTF8(v)) s = cs; Py_DECREF(v); }
        return s;
    };
    const std::string capturedOut = extractStr(capOut);
    const std::string capturedErr = extractStr(capErr);
    Py_DECREF(capOut); Py_DECREF(capErr);
    Py_DECREF(ioMod); Py_DECREF(sys);

    if (s_outputCb && (!capturedOut.empty() || !capturedErr.empty()))
        s_outputCb(capturedOut + capturedErr);

    if (result) {
        Py_DECREF(result);
        return {};
    }

    // Capture exception as a formatted traceback string.
    std::string err;
    PyObject *ptype = nullptr, *pval = nullptr, *ptb = nullptr;
    PyErr_Fetch(&ptype, &pval, &ptb);
    PyErr_NormalizeException(&ptype, &pval, &ptb);

    PyObject* tbMod = PyImport_ImportModule("traceback");
    if (tbMod) {
        PyObject* fmt = PyObject_GetAttrString(tbMod, "format_exception");
        if (fmt) {
            PyObject* lines = PyObject_CallFunctionObjArgs(
                fmt,
                ptype ? ptype : Py_None,
                pval  ? pval  : Py_None,
                ptb   ? ptb   : Py_None,
                nullptr);
            if (lines && PyList_Check(lines)) {
                for (Py_ssize_t i = 0; i < PyList_Size(lines); ++i) {
                    PyObject* line = PyList_GetItem(lines, i);  // borrowed
                    if (const char* s = PyUnicode_AsUTF8(line))
                        err += s;
                }
            }
            Py_XDECREF(lines);
            Py_DECREF(fmt);
        }
        Py_DECREF(tbMod);
    }

    if (err.empty() && pval) {
        if (PyObject* s = PyObject_Str(pval))  {
            if (const char* cs = PyUnicode_AsUTF8(s)) err = cs;
            Py_DECREF(s);
        }
    }

    Py_XDECREF(ptype); Py_XDECREF(pval); Py_XDECREF(ptb);
    return err.empty() ? "Unknown Python error" : err;
}
