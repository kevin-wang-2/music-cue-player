#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "ScriptletEngine.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Static callbacks

static std::function<void()>                   s_goCb;
static std::function<void(const std::string&)> s_selectCb;
static std::function<void(const std::string&)> s_alertCb;
static std::function<bool(const std::string&)> s_confirmCb;
static std::function<void(const std::string&)> s_outputCb;
static std::function<void()>                   s_panicCb;

// --- mcp.error types (created at interpreter init, kept alive globally) ---
static PyObject* s_errCueNotFound     = nullptr;
static PyObject* s_errCueType         = nullptr;
static PyObject* s_errNoMasterContext = nullptr;
static PyObject* s_errInvalidOp       = nullptr;

static std::function<int()>                              s_cueCountCb;
static std::function<ScriptletCueInfo(int)>              s_cueInfoCb;
static std::function<void(int)>                          s_cueSelectCb;
static std::function<void(int)>                          s_cueGoCb;
static std::function<void(int)>                          s_cueStartCb;
static std::function<void(int, double)>                  s_cueArmCb;
static std::function<void(int)>                          s_cueStopCb;
static std::function<void(int)>                          s_cueDisarmCb;
static std::function<void(int, const std::string&)>      s_cueSetNameCb;

static std::function<int(const std::string&, const std::string&, const std::string&)>       s_cueInsertCb;
static std::function<int(int, const std::string&, const std::string&, const std::string&)>  s_cueInsertAtCb;
static std::function<void(int, int, bool)>               s_cueMoveCb;
static std::function<void(int)>                          s_cueDeleteCb;
static std::function<ScriptletMCInfo()>                  s_getMcCb;
static std::function<ScriptletStateInfo()>               s_getStateCb;
static std::function<std::vector<std::pair<int,std::string>>()> s_listInfoCb;
static std::function<int()>                              s_activeListIdCb;
static std::function<void(int)>                          s_switchListCb;

static std::function<int()>                              s_getSampleRateCb;
// (cueIdx, bar, beat) → seconds from cue start, or -1.0 if no MC
static std::function<double(int, int, int)>              s_musicalToSecondsCb;

// ---------------------------------------------------------------------------
// Event callback storage

struct EventCb { PyObject* cb{nullptr}; bool once{false}; };

static std::vector<EventCb> s_cueFiredCbs;
static std::vector<EventCb> s_cueSelectedCbs;
static std::vector<EventCb> s_cueInsertedCbs;
static std::vector<EventCb> s_oscEventCbs;
static std::vector<EventCb> s_midiEventCbs;
static std::vector<std::pair<int, EventCb>> s_musicEventCbs;

static void clearAllEventCallbacks() {
    auto dec = [](std::vector<EventCb>& v) {
        for (auto& e : v) Py_DECREF(e.cb);
        v.clear();
    };
    dec(s_cueFiredCbs);
    dec(s_cueSelectedCbs);
    dec(s_cueInsertedCbs);
    dec(s_oscEventCbs);
    dec(s_midiEventCbs);
    for (auto& [sub, e] : s_musicEventCbs) Py_DECREF(e.cb);
    s_musicEventCbs.clear();
}

// ---------------------------------------------------------------------------
// Forward declarations

static PyObject* getCueMod();  // defined in "Event firing" section below
static PyObject* makeCueObject(PyObject* cueMod, int idx);  // defined in "mcp.cue helpers" below

// Forward declaration — MCPTimeType is defined later but referenced by arm().

extern PyTypeObject MCPTimeType;

// ---------------------------------------------------------------------------
// mcp.cue — Cue extension type

typedef struct {
    PyObject_HEAD
    int index;
} MCPCueObject;

// Guard: Cue cannot be instantiated from Python; only obtainable via C factories.
static PyObject* mcp_cue_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError,
        "Cue cannot be instantiated directly; use mcp.cue.list_cues() or insert_cue()");
    return nullptr;
}

static PyObject* mcp_cue_repr(MCPCueObject* self) {
    if (!s_cueInfoCb) return PyUnicode_FromFormat("<Cue %d>", self->index);
    const auto info = s_cueInfoCb(self->index);
    return PyUnicode_FromFormat("<Cue %s '%s' (%s)>",
                                info.number.c_str(),
                                info.name.c_str(),
                                info.type.c_str());
}

// --- Methods ----------------------------------------------------------------

static PyObject* mcp_cue_select(MCPCueObject* self, PyObject*) {
    if (s_cueSelectCb) s_cueSelectCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_go(MCPCueObject* self, PyObject*) {
    if (s_cueGoCb) s_cueGoCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_start(MCPCueObject* self, PyObject*) {
    if (s_cueStartCb) s_cueStartCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_arm(MCPCueObject* self, PyObject* args) {
    PyObject* timeObj = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &timeObj)) return nullptr;

    double t = -1.0;
    if (timeObj != Py_None) {
        if (!PyObject_IsInstance(timeObj, (PyObject*)&MCPTimeType)) {
            PyErr_SetString(PyExc_TypeError,
                "arm() argument must be a mcp.time.Time object");
            return nullptr;
        }
        // MCPTimeObject layout: PyObject_HEAD, double seconds
        t = *reinterpret_cast<double*>(
                reinterpret_cast<char*>(timeObj) + sizeof(PyObject));
    }
    if (s_cueArmCb) s_cueArmCb(self->index, t);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_stop(MCPCueObject* self, PyObject*) {
    if (s_cueStopCb) s_cueStopCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_disarm(MCPCueObject* self, PyObject*) {
    if (s_cueDisarmCb) s_cueDisarmCb(self->index);
    Py_RETURN_NONE;
}

static PyMethodDef kCueMethods[] = {
    {"select",  (PyCFunction)mcp_cue_select, METH_NOARGS,  "select()  — set as selected cue (cursor only)"},
    {"go",      (PyCFunction)mcp_cue_go,     METH_NOARGS,  "go()  — select this cue and fire it"},
    {"start",   (PyCFunction)mcp_cue_start,  METH_NOARGS,  "start()  — fire without changing selection"},
    {"arm",     (PyCFunction)mcp_cue_arm,    METH_VARARGS, "arm(time: Time = None)  — pre-buffer from position"},
    {"stop",    (PyCFunction)mcp_cue_stop,   METH_NOARGS,  "stop()  — stop all active voices"},
    {"disarm",  (PyCFunction)mcp_cue_disarm, METH_NOARGS,  "disarm()  — release pre-buffered audio"},
    {nullptr}
};

// --- Properties (getset) ----------------------------------------------------

static inline bool getInfo(MCPCueObject* self, ScriptletCueInfo& out) {
    if (!s_cueInfoCb) { PyErr_SetString(PyExc_RuntimeError, "no cue info callback"); return false; }
    out = s_cueInfoCb(self->index);
    return true;
}

#define CUE_RO(prop, expr) \
    static PyObject* mcp_cue_get_##prop(MCPCueObject* self, void*) { \
        ScriptletCueInfo _i; if (!getInfo(self, _i)) return nullptr; \
        return (expr); \
    }

CUE_RO(number,       PyUnicode_FromString(_i.number.c_str()))
CUE_RO(type,         PyUnicode_FromString(_i.type.c_str()))
CUE_RO(pre_wait,     PyFloat_FromDouble(_i.preWait))
CUE_RO(auto_continue,PyBool_FromLong(_i.autoContinue))
CUE_RO(auto_follow,  PyBool_FromLong(_i.autoFollow))
CUE_RO(is_playing,   PyBool_FromLong(_i.isPlaying))
CUE_RO(is_pending,   PyBool_FromLong(_i.isPending))
CUE_RO(is_armed,     PyBool_FromLong(_i.isArmed))
CUE_RO(path,         PyUnicode_FromString(_i.path.c_str()))
CUE_RO(level,        PyFloat_FromDouble(_i.level))
CUE_RO(trim,         PyFloat_FromDouble(_i.trim))
CUE_RO(start_time,   PyFloat_FromDouble(_i.startTime))
CUE_RO(duration,     PyFloat_FromDouble(_i.duration))
CUE_RO(playhead,     PyFloat_FromDouble(_i.playhead))
CUE_RO(target_index, PyLong_FromLong(_i.targetIndex))
CUE_RO(target_number,PyUnicode_FromString(_i.targetNumber.c_str()))
CUE_RO(code,         PyUnicode_FromString(_i.code.c_str()))

static PyObject* mcp_cue_get_index_direct(MCPCueObject* self, void*) {
    return PyLong_FromLong(self->index);
}
static PyObject* mcp_cue_get_name(MCPCueObject* self, void*) {
    ScriptletCueInfo i; if (!getInfo(self, i)) return nullptr;
    return PyUnicode_FromString(i.name.c_str());
}
static int mcp_cue_set_name(MCPCueObject* self, PyObject* val, void*) {
    if (!val || !PyUnicode_Check(val)) {
        PyErr_SetString(PyExc_TypeError, "name must be a str"); return -1;
    }
    const char* s = PyUnicode_AsUTF8(val);
    if (!s) return -1;
    if (s_cueSetNameCb) s_cueSetNameCb(self->index, s);
    return 0;
}

#define RO(name, getter, doc) \
    {const_cast<char*>(name), (getter_func)(getter), nullptr, const_cast<char*>(doc), nullptr}
#define RW(name, getter, setter, doc) \
    {const_cast<char*>(name), (getter_func)(getter), (setter_func)(setter), const_cast<char*>(doc), nullptr}

using getter_func = PyObject* (*)(PyObject*, void*);
using setter_func = int       (*)(PyObject*, PyObject*, void*);

static PyGetSetDef kCueGetSet[] = {
    RO("index",         mcp_cue_get_index_direct, "0-based cue index"),
    RO("number",        mcp_cue_get_number,       "cue number string"),
    RW("name",          mcp_cue_get_name,         mcp_cue_set_name, "cue name"),
    RO("type",          mcp_cue_get_type,         "cue type string"),
    RO("pre_wait",      mcp_cue_get_pre_wait,     "pre-wait in seconds"),
    RO("auto_continue", mcp_cue_get_auto_continue,"True if auto-continue enabled"),
    RO("auto_follow",   mcp_cue_get_auto_follow,  "True if auto-follow enabled"),
    RO("is_playing",    mcp_cue_get_is_playing,   "True if cue has active voices"),
    RO("is_pending",    mcp_cue_get_is_pending,   "True if in pre-wait"),
    RO("is_armed",      mcp_cue_get_is_armed,     "True if pre-buffered"),
    RO("path",          mcp_cue_get_path,         "audio file path (AudioCue only)"),
    RO("level",         mcp_cue_get_level,        "output level dB (AudioCue only)"),
    RO("trim",          mcp_cue_get_trim,         "trim dB (AudioCue only)"),
    RO("start_time",    mcp_cue_get_start_time,   "playback start offset (s)"),
    RO("duration",      mcp_cue_get_duration,     "playback duration (s)"),
    RO("playhead",      mcp_cue_get_playhead,     "current file-position playhead (s)"),
    RO("target_index",  mcp_cue_get_target_index, "target cue index (-1 if none)"),
    RO("target_number", mcp_cue_get_target_number,"target cue number string"),
    RO("code",          mcp_cue_get_code,         "Python source (ScriptletCue only)"),
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyTypeObject MCPCueType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.cue.Cue",
    sizeof(MCPCueObject),
    0,
    nullptr,                                        // tp_dealloc (default handles refcnt)
    0, nullptr, nullptr, nullptr,
    (reprfunc)mcp_cue_repr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr,
    nullptr,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    "Cue — abstract cue handle (obtain via list_cues / insert_cue)",
    nullptr, nullptr, nullptr,
    0, nullptr, nullptr,
    kCueMethods,
    nullptr,
    kCueGetSet,
    nullptr, nullptr,
    nullptr, nullptr, 0,
    nullptr,                                        // tp_init — not used; index set by C factory
    nullptr,                                        // tp_alloc (default)
    mcp_cue_new_guard,                              // tp_new — blocks Python-side construction
};
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// mcp.time — Time value type

typedef struct {
    PyObject_HEAD
    double seconds;
} MCPTimeObject;

static PyObject* mcp_time_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError,
        "Time cannot be instantiated directly; use Time.from_sample(), from_min_sec(), etc.");
    return nullptr;
}

static PyObject* mcp_time_repr(MCPTimeObject* self) {
    int h  = (int)(self->seconds / 3600.0);
    int m  = (int)((self->seconds - h * 3600.0) / 60.0);
    double s = self->seconds - h * 3600.0 - m * 60.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "<Time %02d:%02d:%06.3f>", h, m, s);
    return PyUnicode_FromString(buf);
}

// --- Instance methods -------------------------------------------------------

static PyObject* mcp_time_to_seconds(MCPTimeObject* self, PyObject*) {
    return PyFloat_FromDouble(self->seconds);
}
static PyObject* mcp_time_to_samples(MCPTimeObject* self, PyObject*) {
    const int sr = (s_getSampleRateCb && s_getSampleRateCb() > 0)
                   ? s_getSampleRateCb() : 44100;
    return PyLong_FromLongLong(static_cast<long long>(self->seconds * sr));
}
static PyObject* mcp_time_to_min_sec(MCPTimeObject* self, PyObject*) {
    const int    min = static_cast<int>(self->seconds / 60.0);
    const double sec = self->seconds - min * 60.0;
    return Py_BuildValue("(id)", min, sec);
}
static PyObject* mcp_time_to_timecode(MCPTimeObject* self, PyObject* args) {
    double fps = 25.0;
    if (!PyArg_ParseTuple(args, "|d", &fps)) return nullptr;
    if (fps <= 0) fps = 25.0;
    const double total = self->seconds;
    const int h = static_cast<int>(total / 3600.0);
    const int m = static_cast<int>((total - h * 3600.0) / 60.0);
    const int s = static_cast<int>(total - h * 3600.0 - m * 60.0);
    const int f = static_cast<int>(std::fmod(total, 1.0) * fps);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", h, m, s, f);
    return PyUnicode_FromString(buf);
}

// Forward declarations for static factory methods (defined below makeTimeObject).
static PyObject* mcp_time_from_sample   (PyObject*, PyObject*);
static PyObject* mcp_time_from_min_sec  (PyObject*, PyObject*);
static PyObject* mcp_time_from_timecode (PyObject*, PyObject*);
static PyObject* mcp_time_from_bar_beat (PyObject*, PyObject*);

static PyMethodDef kTimeMethods[] = {
    // Instance methods
    {"to_seconds",    (PyCFunction)mcp_time_to_seconds,    METH_NOARGS,                  "to_seconds() → float"},
    {"to_samples",    (PyCFunction)mcp_time_to_samples,    METH_NOARGS,                  "to_samples() → int"},
    {"to_min_sec",    (PyCFunction)mcp_time_to_min_sec,    METH_NOARGS,                  "to_min_sec() → (int, float)"},
    {"to_timecode",   (PyCFunction)mcp_time_to_timecode,   METH_VARARGS,                 "to_timecode(fps=25) → 'HH:MM:SS:FF'"},
    // Static factory methods (PyType_Ready wraps these in staticmethod descriptors)
    {"from_sample",   (PyCFunction)mcp_time_from_sample,   METH_VARARGS | METH_STATIC,   "from_sample(n) → Time"},
    {"from_min_sec",  (PyCFunction)mcp_time_from_min_sec,  METH_VARARGS | METH_STATIC,   "from_min_sec(min, sec) → Time"},
    {"from_timecode", (PyCFunction)mcp_time_from_timecode, METH_VARARGS | METH_STATIC,   "from_timecode(tc, fps=25) → Time"},
    {"from_bar_beat", (PyCFunction)mcp_time_from_bar_beat, METH_VARARGS | METH_STATIC,   "from_bar_beat(bar, beat, cue) → Time"},
    {nullptr}
};

// Helper: allocate a MCPTimeObject and set its seconds field.
// Bypasses tp_new guard — the only way to create Time objects.
static PyObject* makeTimeObject(double seconds) {
    MCPTimeObject* obj = (MCPTimeObject*)MCPTimeType.tp_alloc(&MCPTimeType, 0);
    if (!obj) return nullptr;
    obj->seconds = seconds;
    return (PyObject*)obj;
}

static PyObject* mcp_time_from_sample(PyObject*, PyObject* args) {
    long long sample = 0;
    if (!PyArg_ParseTuple(args, "L", &sample)) return nullptr;
    const int sr = (s_getSampleRateCb && s_getSampleRateCb() > 0)
                   ? s_getSampleRateCb() : 44100;
    return makeTimeObject(static_cast<double>(sample) / sr);
}
static PyObject* mcp_time_from_min_sec(PyObject*, PyObject* args) {
    int min = 0; double sec = 0.0;
    if (!PyArg_ParseTuple(args, "id", &min, &sec)) return nullptr;
    return makeTimeObject(min * 60.0 + sec);
}
static PyObject* mcp_time_from_timecode(PyObject*, PyObject* args) {
    const char* tc = nullptr; double fps = 25.0;
    if (!PyArg_ParseTuple(args, "s|d", &tc, &fps)) return nullptr;
    if (!tc || fps <= 0) { PyErr_SetString(PyExc_ValueError, "invalid timecode or fps"); return nullptr; }
    // Accept HH:MM:SS:FF or MM:SS:FF
    int a = 0, b = 0, c = 0, d = 0;
    const int n = std::sscanf(tc, "%d:%d:%d:%d", &a, &b, &c, &d);
    double secs = 0.0;
    if (n == 4)      secs = a * 3600.0 + b * 60.0 + c + d / fps;
    else if (n == 3) secs = a * 60.0   + b        + c / fps;
    else { PyErr_SetString(PyExc_ValueError, "timecode must be HH:MM:SS:FF or MM:SS:FF"); return nullptr; }
    return makeTimeObject(secs);
}
static PyObject* mcp_time_from_bar_beat(PyObject*, PyObject* args) {
    int bar = 1, beat = 1;
    PyObject* cueObj = nullptr;
    if (!PyArg_ParseTuple(args, "iiO", &bar, &beat, &cueObj)) return nullptr;
    if (!PyObject_IsInstance(cueObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "cue must be a mcp.cue.Cue"); return nullptr;
    }
    const int cueIdx = ((MCPCueObject*)cueObj)->index;
    if (!s_musicalToSecondsCb) {
        PyErr_SetString(PyExc_RuntimeError, "no musical-time callback"); return nullptr;
    }
    const double secs = s_musicalToSecondsCb(cueIdx, bar, beat);
    if (secs < 0.0) {
        PyObject* exc = s_errNoMasterContext ? s_errNoMasterContext : PyExc_ValueError;
        PyErr_SetString(exc, "cue has no music context"); return nullptr;
    }
    return makeTimeObject(secs);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject MCPTimeType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.time.Time",
    sizeof(MCPTimeObject),
    0,
    nullptr,
    0, nullptr, nullptr, nullptr,
    (reprfunc)mcp_time_repr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr,
    nullptr,
    Py_TPFLAGS_DEFAULT,
    "Time — immutable time value with conversion utilities",
    nullptr, nullptr, nullptr,
    0, nullptr, nullptr,
    kTimeMethods,
    nullptr, nullptr,
    nullptr, nullptr,
    nullptr, nullptr, 0,
    nullptr,
    nullptr,
    mcp_time_new_guard,
};
#pragma GCC diagnostic pop

static PyMethodDef kTimeModuleMethods[] = { {nullptr} };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kTimeModuleDef = {
    PyModuleDef_HEAD_INIT, "mcp.time", "mcp time utilities", -1, kTimeModuleMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp_time() {
    if (PyType_Ready(&MCPTimeType) < 0) return nullptr;

    PyObject* mod = PyModule_Create(&kTimeModuleDef);
    if (!mod) return nullptr;

    Py_INCREF(&MCPTimeType);
    PyModule_AddObject(mod, "Time", (PyObject*)&MCPTimeType);
    return mod;
}

// ---------------------------------------------------------------------------
// mcp.cue helpers

static const char* classForType(const std::string& t) {
    if (t == "audio")         return "AudioCue";
    if (t == "start")         return "StartCue";
    if (t == "stop")          return "StopCue";
    if (t == "arm")           return "ArmCue";
    if (t == "fade")          return "FadeCue";
    if (t == "devamp")        return "DevampCue";
    if (t == "goto")          return "GotoCue";
    if (t == "memo")          return "MemoCue";
    if (t == "scriptlet")     return "ScriptletCue";
    if (t == "network")       return "NetworkCue";
    if (t == "midi")          return "MidiCue";
    if (t == "timecode")      return "TimecodeCue";
    if (t == "marker")        return "MarkerCue";
    if (t == "group")         return "GroupCue";
    return "Cue";
}

// Allocate a Cue instance directly, bypassing tp_new guard.
// Resolves the concrete subclass from cueMod when available.
static PyObject* makeCueObject(PyObject* cueMod, int idx) {
    PyTypeObject* type = &MCPCueType;

    if (cueMod && s_cueInfoCb) {
        const auto info    = s_cueInfoCb(idx);
        const char* clsName = classForType(info.type);
        PyObject* cls = PyObject_GetAttrString(cueMod, clsName);
        if (cls) {
            type = (PyTypeObject*)cls;
            Py_DECREF(cls);
        } else {
            PyErr_Clear();
        }
    }

    MCPCueObject* obj = (MCPCueObject*)type->tp_alloc(type, 0);
    if (!obj) return nullptr;
    obj->index = idx;
    return (PyObject*)obj;
}

// ---------------------------------------------------------------------------
// mcp.cue module functions

static PyObject* mcp_cue_list_cues(PyObject* module, PyObject*) {
    if (!s_cueCountCb || !s_cueInfoCb) return PyList_New(0);

    const int count = s_cueCountCb();
    PyObject* result = PyList_New(count);
    if (!result) return nullptr;

    for (int i = 0; i < count; ++i) {
        PyObject* obj = makeCueObject(module, i);
        if (!obj) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, i, obj);
    }
    return result;
}

static PyObject* mcp_cue_insert_cue(PyObject* module, PyObject* args) {
    const char* type   = nullptr;
    const char* number = "";
    const char* name   = "";
    if (!PyArg_ParseTuple(args, "s|ss", &type, &number, &name)) return nullptr;
    if (!s_cueInsertCb) { PyErr_SetString(PyExc_RuntimeError, "no insert callback"); return nullptr; }

    const int newIdx = s_cueInsertCb(type   ? type   : "",
                                      number ? number : "",
                                      name   ? name   : "");
    if (newIdx < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue failed"); return nullptr; }
    return makeCueObject(module, newIdx);
}

static PyObject* mcp_cue_insert_cue_at(PyObject* module, PyObject* args) {
    PyObject*   refObj = nullptr;
    const char* type   = nullptr;
    const char* number = "";
    const char* name   = "";
    if (!PyArg_ParseTuple(args, "Os|ss", &refObj, &type, &number, &name)) return nullptr;

    if (!PyObject_IsInstance(refObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "ref must be a Cue"); return nullptr;
    }
    const int refIdx = ((MCPCueObject*)refObj)->index;
    if (!s_cueInsertAtCb) { PyErr_SetString(PyExc_RuntimeError, "no insert_at callback"); return nullptr; }

    const int newIdx = s_cueInsertAtCb(refIdx,
                                        type   ? type   : "",
                                        number ? number : "",
                                        name   ? name   : "");
    if (newIdx < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue_at failed"); return nullptr; }
    return makeCueObject(module, newIdx);
}

static PyObject* mcp_cue_move_cue_at(PyObject* /*module*/, PyObject* args) {
    PyObject* refObj = nullptr;
    PyObject* cueObj = nullptr;
    int       toGroup = 0;
    if (!PyArg_ParseTuple(args, "OO|p", &refObj, &cueObj, &toGroup)) return nullptr;

    if (!PyObject_IsInstance(refObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "ref must be a Cue"); return nullptr;
    }
    if (!PyObject_IsInstance(cueObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "cue must be a Cue"); return nullptr;
    }
    if (!s_cueMoveCb) { PyErr_SetString(PyExc_RuntimeError, "no move callback"); return nullptr; }
    s_cueMoveCb(((MCPCueObject*)refObj)->index,
                ((MCPCueObject*)cueObj)->index,
                toGroup != 0);
    Py_RETURN_NONE;
}

static PyObject* mcp_cue_delete_cue(PyObject* /*module*/, PyObject* arg) {
    if (!PyObject_IsInstance(arg, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "arg must be a Cue"); return nullptr;
    }
    const int idx = ((MCPCueObject*)arg)->index;
    if (!s_cueDeleteCb) { PyErr_SetString(PyExc_RuntimeError, "no delete callback"); return nullptr; }
    s_cueDeleteCb(idx);
    ((MCPCueObject*)arg)->index = -1;
    Py_RETURN_NONE;
}

static PyMethodDef kCueModuleMethods[] = {
    {"list_cues",     mcp_cue_list_cues,     METH_NOARGS,  "list_cues() → [Cue]"},
    {"insert_cue",    mcp_cue_insert_cue,    METH_VARARGS, "insert_cue(type, number='', name='') → Cue"},
    {"insert_cue_at", mcp_cue_insert_cue_at, METH_VARARGS, "insert_cue_at(ref, type, number='', name='') → Cue"},
    {"move_cue_at",   mcp_cue_move_cue_at,   METH_VARARGS, "move_cue_at(ref, cue, to_group=False)"},
    {"delete_cue",    mcp_cue_delete_cue,    METH_O,       "delete_cue(cue)"},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kCueModuleDef = {
    PyModuleDef_HEAD_INIT, "mcp.cue", "mcp cue API", -1, kCueModuleMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp_cue() {
    if (PyType_Ready(&MCPCueType) < 0) return nullptr;

    PyObject* mod = PyModule_Create(&kCueModuleDef);
    if (!mod) return nullptr;

    Py_INCREF(&MCPCueType);
    PyModule_AddObject(mod, "Cue", (PyObject*)&MCPCueType);

    static const char* kSubclasses = R"py(
AudioCue    = type('AudioCue',    (Cue,), {'__doc__': 'Audio playback cue'})
StartCue    = type('StartCue',    (Cue,), {'__doc__': 'Start/trigger another cue'})
StopCue     = type('StopCue',     (Cue,), {'__doc__': 'Stop another cue'})
ArmCue      = type('ArmCue',      (Cue,), {'__doc__': 'Pre-buffer another cue'})
FadeCue     = type('FadeCue',     (Cue,), {'__doc__': 'Fade another cue'})
DevampCue   = type('DevampCue',   (Cue,), {'__doc__': 'Exit-vamp / devamp cue'})
GotoCue     = type('GotoCue',     (Cue,), {'__doc__': 'Jump to a cue number'})
MemoCue     = type('MemoCue',     (Cue,), {'__doc__': 'Memo / label cue'})
ScriptletCue= type('ScriptletCue',(Cue,), {'__doc__': 'Python scriptlet cue'})
NetworkCue  = type('NetworkCue',  (Cue,), {'__doc__': 'Network output cue'})
MidiCue     = type('MidiCue',     (Cue,), {'__doc__': 'MIDI output cue'})
TimecodeCue = type('TimecodeCue', (Cue,), {'__doc__': 'LTC/MTC timecode cue'})
MarkerCue   = type('MarkerCue',   (Cue,), {'__doc__': 'Start from a time marker'})
GroupCue    = type('GroupCue',    (Cue,), {'__doc__': 'Group / timeline cue'})
)py";

    PyObject* modDict = PyModule_GetDict(mod);
    PyObject* res = PyRun_String(kSubclasses, Py_file_input, modDict, modDict);
    if (res) { Py_DECREF(res); } else { PyErr_Clear(); }

    return mod;
}

// ---------------------------------------------------------------------------
// mcp.event module

static PyObject* eventRegister(std::vector<EventCb>& vec, PyObject* arg, bool once) {
    if (!PyCallable_Check(arg)) { PyErr_SetString(PyExc_TypeError, "callback must be callable"); return nullptr; }
    Py_INCREF(arg);
    vec.push_back({arg, once});
    Py_RETURN_NONE;
}

static PyObject* mcp_event_on_cue_fired    (PyObject*, PyObject* a) { return eventRegister(s_cueFiredCbs,    a, false); }
static PyObject* mcp_event_once_cue_fired  (PyObject*, PyObject* a) { return eventRegister(s_cueFiredCbs,    a, true);  }
static PyObject* mcp_event_on_cue_selected (PyObject*, PyObject* a) { return eventRegister(s_cueSelectedCbs, a, false); }
static PyObject* mcp_event_once_cue_selected(PyObject*,PyObject* a) { return eventRegister(s_cueSelectedCbs, a, true);  }
static PyObject* mcp_event_on_cue_inserted (PyObject*, PyObject* a) { return eventRegister(s_cueInsertedCbs, a, false); }
static PyObject* mcp_event_once_cue_inserted(PyObject*,PyObject* a) { return eventRegister(s_cueInsertedCbs, a, true);  }
static PyObject* mcp_event_on_osc_event    (PyObject*, PyObject* a) { return eventRegister(s_oscEventCbs,    a, false); }
static PyObject* mcp_event_once_osc_event  (PyObject*, PyObject* a) { return eventRegister(s_oscEventCbs,    a, true);  }
static PyObject* mcp_event_on_midi_event   (PyObject*, PyObject* a) { return eventRegister(s_midiEventCbs,   a, false); }
static PyObject* mcp_event_once_midi_event (PyObject*, PyObject* a) { return eventRegister(s_midiEventCbs,   a, true);  }

static PyObject* mcp_event_on_music_event_impl(PyObject*, PyObject* args, bool once) {
    const char* quant = nullptr;
    PyObject*   cb    = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &quant, &cb)) return nullptr;
    if (!PyCallable_Check(cb)) { PyErr_SetString(PyExc_TypeError, "callback must be callable"); return nullptr; }
    int sub = 0;
    if      (std::strcmp(quant, "1/1") == 0)  sub = 1;
    else if (std::strcmp(quant, "1/2") == 0)  sub = 2;
    else if (std::strcmp(quant, "1/4") == 0)  sub = 4;
    else if (std::strcmp(quant, "1/8") == 0)  sub = 8;
    else if (std::strcmp(quant, "1/16") == 0) sub = 16;
    else { PyErr_SetString(PyExc_ValueError, "quantization must be '1/1','1/2','1/4','1/8','1/16'"); return nullptr; }
    Py_INCREF(cb);
    s_musicEventCbs.push_back({sub, {cb, once}});
    Py_RETURN_NONE;
}
static PyObject* mcp_event_on_music_event  (PyObject* m, PyObject* a) { return mcp_event_on_music_event_impl(m, a, false); }
static PyObject* mcp_event_once_music_event(PyObject* m, PyObject* a) { return mcp_event_on_music_event_impl(m, a, true);  }

static PyObject* mcp_event_clear_all(PyObject*, PyObject*) {
    clearAllEventCallbacks();
    Py_RETURN_NONE;
}

static PyMethodDef kEventMethods[] = {
    {"on_cue_fired",         mcp_event_on_cue_fired,          METH_O,       "on_cue_fired(cb)"},
    {"once_cue_fired",       mcp_event_once_cue_fired,        METH_O,       "once_cue_fired(cb)"},
    {"on_cue_selected",      mcp_event_on_cue_selected,       METH_O,       "on_cue_selected(cb)"},
    {"once_cue_selected",    mcp_event_once_cue_selected,     METH_O,       "once_cue_selected(cb)"},
    {"on_cue_inserted",      mcp_event_on_cue_inserted,       METH_O,       "on_cue_inserted(cb)"},
    {"once_cue_inserted",    mcp_event_once_cue_inserted,     METH_O,       "once_cue_inserted(cb)"},
    {"on_osc_event",         mcp_event_on_osc_event,          METH_O,       "on_osc_event(cb)"},
    {"once_osc_event",       mcp_event_once_osc_event,        METH_O,       "once_osc_event(cb)"},
    {"on_midi_event",        mcp_event_on_midi_event,         METH_O,       "on_midi_event(cb)"},
    {"once_midi_event",      mcp_event_once_midi_event,       METH_O,       "once_midi_event(cb)"},
    {"on_music_event",       mcp_event_on_music_event,        METH_VARARGS, "on_music_event(quantization, cb)"},
    {"once_music_event",     mcp_event_once_music_event,      METH_VARARGS, "once_music_event(quantization, cb)"},
    {"clear_all",            mcp_event_clear_all,             METH_NOARGS,  "clear_all()"},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kEventModuleDef = {
    PyModuleDef_HEAD_INIT, "mcp.event", "mcp event subscriptions", -1, kEventMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp_event() { return PyModule_Create(&kEventModuleDef); }

// ---------------------------------------------------------------------------
// mcp.error module

static PyObject* PyInit_mcp_error() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    static PyModuleDef def = { PyModuleDef_HEAD_INIT, "mcp.error", "mcp error types", -1, nullptr };
#pragma GCC diagnostic pop
    PyObject* mod = PyModule_Create(&def);
    if (!mod) return nullptr;

    auto addErr = [&](PyObject*& slot, const char* fullName, const char* shortName) {
        slot = PyErr_NewException(fullName, nullptr, nullptr);
        if (!slot) return;
        Py_INCREF(slot);  // one ref for global slot, one stolen by AddObject
        PyModule_AddObject(mod, shortName, slot);
    };

    addErr(s_errCueNotFound,     "mcp.error.CueNotFoundError",     "CueNotFoundError");
    addErr(s_errCueType,         "mcp.error.CueTypeError",         "CueTypeError");
    addErr(s_errNoMasterContext, "mcp.error.NoMasterContextError", "NoMasterContextError");
    addErr(s_errInvalidOp,       "mcp.error.InvalidOperationError","InvalidOperationError");

    return mod;
}

// ---------------------------------------------------------------------------
// mcp module (top-level)

static PyObject* py_go(PyObject*, PyObject*) { if (s_goCb) s_goCb(); Py_RETURN_NONE; }
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
static PyObject* py_confirm(PyObject*, PyObject* args) {
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
    if (s_confirmCb && msg) return PyBool_FromLong(s_confirmCb(msg) ? 1 : 0);
    Py_RETURN_FALSE;
}
static PyObject* py_panic(PyObject*, PyObject*) { if (s_panicCb) s_panicCb(); Py_RETURN_NONE; }

static PyObject* py_get_mc(PyObject*, PyObject*) {
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    ScriptletMCInfo info;
    if (s_getMcCb) info = s_getMcCb();
    auto setI = [&](const char* k, PyObject* v) { PyDict_SetItemString(d, k, v); Py_DECREF(v); };
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%d/%d", info.timeSigNum, info.timeSigDen);
    setI("bpm",            PyFloat_FromDouble(info.valid ? info.bpm : 0.0));
    setI("time_signature", PyUnicode_FromString(info.valid ? ts : ""));
    setI("bar",            PyLong_FromLong(info.valid ? info.bar  : 0));
    setI("beat",           PyLong_FromLong(info.valid ? info.beat : 0));
    setI("PPQ",            PyLong_FromLong(480));
    return d;
}

static PyObject* py_get_state(PyObject*, PyObject*) {
    ScriptletStateInfo state;
    if (s_getStateCb) state = s_getStateCb();

    PyObject* cueMod = getCueMod();

    // selected_cue
    PyObject* selCue = Py_None;
    if (state.selectedCue >= 0) {
        selCue = makeCueObject(cueMod, state.selectedCue);
        if (!selCue) { PyErr_Clear(); selCue = Py_None; }
    }

    // running_cues list
    PyObject* runList = PyList_New(static_cast<Py_ssize_t>(state.runningCues.size()));
    if (!runList) { if (selCue != Py_None) Py_DECREF(selCue); return nullptr; }
    for (size_t i = 0; i < state.runningCues.size(); ++i) {
        PyObject* obj = makeCueObject(cueMod, state.runningCues[i]);
        if (!obj) { PyErr_Clear(); obj = Py_None; Py_INCREF(obj); }
        PyList_SET_ITEM(runList, static_cast<Py_ssize_t>(i), obj);
    }

    // mc_master
    PyObject* mcMaster = Py_None;
    if (state.mcMaster >= 0) {
        mcMaster = makeCueObject(cueMod, state.mcMaster);
        if (!mcMaster) { PyErr_Clear(); mcMaster = Py_None; }
    }

    PyObject* d = PyDict_New();
    if (!d) {
        Py_DECREF(runList);
        if (selCue  != Py_None) Py_DECREF(selCue);
        if (mcMaster != Py_None) Py_DECREF(mcMaster);
        return nullptr;
    }
    PyDict_SetItemString(d, "selected_cue",  selCue  != Py_None ? selCue  : Py_None);
    PyDict_SetItemString(d, "running_cues",  runList);
    PyDict_SetItemString(d, "mc_master",     mcMaster != Py_None ? mcMaster : Py_None);
    if (selCue  != Py_None) Py_DECREF(selCue);
    if (mcMaster != Py_None) Py_DECREF(mcMaster);
    Py_DECREF(runList);
    return d;
}

static PyObject* py_list_lists(PyObject*, PyObject*) {
    if (!s_listInfoCb) return PyList_New(0);
    const auto lists = s_listInfoCb();
    PyObject* result = PyList_New(static_cast<Py_ssize_t>(lists.size()));
    if (!result) return nullptr;
    for (size_t i = 0; i < lists.size(); ++i) {
        PyObject* d = PyDict_New();
        PyObject* id   = PyLong_FromLong(lists[i].first);
        PyObject* name = PyUnicode_FromString(lists[i].second.c_str());
        PyDict_SetItemString(d, "id",   id);
        PyDict_SetItemString(d, "name", name);
        Py_DECREF(id); Py_DECREF(name);
        PyList_SET_ITEM(result, static_cast<Py_ssize_t>(i), d);
    }
    return result;
}

static PyObject* py_get_active_list(PyObject*, PyObject*) {
    return PyLong_FromLong(s_activeListIdCb ? s_activeListIdCb() : -1);
}

static PyObject* py_switch_list(PyObject*, PyObject* args) {
    int listId;
    if (!PyArg_ParseTuple(args, "i", &listId)) return nullptr;
    if (s_switchListCb) s_switchListCb(listId);
    Py_RETURN_NONE;
}

static PyMethodDef kMcpMethods[] = {
    {"go",              py_go,              METH_NOARGS,  "go()"},
    {"select",          py_select,          METH_VARARGS, "select(num)"},
    {"alert",           py_alert,           METH_VARARGS, "alert(msg)"},
    {"confirm",         py_confirm,         METH_VARARGS, "confirm(msg) → bool"},
    {"panic",           py_panic,           METH_NOARGS,  "panic()"},
    {"get_mc",          py_get_mc,          METH_NOARGS,  "get_mc() → dict"},
    {"get_state",       py_get_state,       METH_NOARGS,  "get_state() → dict"},
    {"list_lists",      py_list_lists,      METH_NOARGS,  "list_lists() → [{id, name}]"},
    {"get_active_list", py_get_active_list, METH_NOARGS,  "get_active_list() → int"},
    {"switch_list",     py_switch_list,     METH_VARARGS, "switch_list(list_id)"},
    {nullptr, nullptr, 0, nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kMcpModule = {
    PyModuleDef_HEAD_INIT, "mcp", "Music Cue Player scripting API", -1, kMcpMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp() {
    PyObject* mod = PyModule_Create(&kMcpModule);
    if (!mod) return nullptr;

    PyObject* sysModules = PySys_GetObject("modules");

    auto registerSub = [&](const char* name, PyObject* sub) {
        if (!sub) return;
        if (sysModules) PyDict_SetItemString(sysModules, name, sub);
        PyModule_AddObject(mod, std::strrchr(name, '.') + 1, sub);  // steals
    };

    registerSub("mcp.cue",   PyInit_mcp_cue());
    registerSub("mcp.event", PyInit_mcp_event());
    registerSub("mcp.time",  PyInit_mcp_time());
    registerSub("mcp.error", PyInit_mcp_error());

    return mod;
}

// ---------------------------------------------------------------------------
// ScriptletEngine

ScriptletEngine::ScriptletEngine() {
    PyImport_AppendInittab("mcp", &PyInit_mcp);
    Py_Initialize();
    m_initialized = true;
}
ScriptletEngine::~ScriptletEngine() {
    if (m_initialized) { clearAllEventCallbacks(); Py_FinalizeEx(); }
}

void ScriptletEngine::setGoCallback      (std::function<void()> cb)                    { s_goCb        = std::move(cb); }
void ScriptletEngine::setSelectCallback  (std::function<void(const std::string&)> cb)  { s_selectCb    = std::move(cb); }
void ScriptletEngine::setAlertCallback   (std::function<void(const std::string&)> cb)  { s_alertCb     = std::move(cb); }
void ScriptletEngine::setConfirmCallback (std::function<bool(const std::string&)> cb)  { s_confirmCb   = std::move(cb); }
void ScriptletEngine::setOutputCallback  (std::function<void(const std::string&)> cb)  { s_outputCb    = std::move(cb); }
void ScriptletEngine::setPanicCallback   (std::function<void()> cb)                    { s_panicCb     = std::move(cb); }

void ScriptletEngine::setCueCountCallback  (std::function<int()> cb)                              { s_cueCountCb   = std::move(cb); }
void ScriptletEngine::setCueInfoCallback   (std::function<ScriptletCueInfo(int)> cb)              { s_cueInfoCb    = std::move(cb); }
void ScriptletEngine::setCueSelectCallback (std::function<void(int)> cb)                          { s_cueSelectCb  = std::move(cb); }
void ScriptletEngine::setCueGoCallback     (std::function<void(int)> cb)                          { s_cueGoCb      = std::move(cb); }
void ScriptletEngine::setCueStartCallback  (std::function<void(int)> cb)                          { s_cueStartCb   = std::move(cb); }
void ScriptletEngine::setCueArmCallback    (std::function<void(int, double)> cb)                  { s_cueArmCb     = std::move(cb); }
void ScriptletEngine::setCueStopCallback   (std::function<void(int)> cb)                          { s_cueStopCb    = std::move(cb); }
void ScriptletEngine::setCueDisarmCallback (std::function<void(int)> cb)                          { s_cueDisarmCb  = std::move(cb); }
void ScriptletEngine::setCueSetNameCallback(std::function<void(int, const std::string&)> cb)      { s_cueSetNameCb = std::move(cb); }

void ScriptletEngine::setCueInsertCallback  (std::function<int(const std::string&, const std::string&, const std::string&)> cb)      { s_cueInsertCb   = std::move(cb); }
void ScriptletEngine::setCueInsertAtCallback(std::function<int(int, const std::string&, const std::string&, const std::string&)> cb) { s_cueInsertAtCb = std::move(cb); }
void ScriptletEngine::setCueMoveCallback    (std::function<void(int, int, bool)> cb)                                                  { s_cueMoveCb     = std::move(cb); }
void ScriptletEngine::setCueDeleteCallback  (std::function<void(int)> cb)                                                             { s_cueDeleteCb   = std::move(cb); }
void ScriptletEngine::setGetMCCallback      (std::function<ScriptletMCInfo()> cb)                                                     { s_getMcCb       = std::move(cb); }
void ScriptletEngine::setGetStateCallback   (std::function<ScriptletStateInfo()> cb)                                                  { s_getStateCb    = std::move(cb); }

void ScriptletEngine::setGetSampleRateCallback    (std::function<int()> cb)                       { s_getSampleRateCb     = std::move(cb); }
void ScriptletEngine::setMusicalToSecondsCallback (std::function<double(int, int, int)> cb)       { s_musicalToSecondsCb  = std::move(cb); }

void ScriptletEngine::setListInfoCallback    (std::function<std::vector<std::pair<int,std::string>>()> cb) { s_listInfoCb     = std::move(cb); }
void ScriptletEngine::setActiveListIdCallback(std::function<int()>     cb)                                 { s_activeListIdCb = std::move(cb); }
void ScriptletEngine::setSwitchListCallback  (std::function<void(int)> cb)                                 { s_switchListCb   = std::move(cb); }

// ---------------------------------------------------------------------------
// Event firing

static PyObject* getCueMod() {
    PyObject* sm = PySys_GetObject("modules");
    return sm ? PyDict_GetItemString(sm, "mcp.cue") : nullptr;
}

static void fireCueCbList(std::vector<EventCb>& cbs, PyObject* arg) {
    auto it = cbs.begin();
    while (it != cbs.end()) {
        PyObject* r = PyObject_CallFunction(it->cb, "O", arg);
        if (!r) PyErr_Clear(); else Py_DECREF(r);
        if (it->once) { Py_DECREF(it->cb); it = cbs.erase(it); } else ++it;
    }
}

void ScriptletEngine::fireCueFiredEvent(int idx) {
    if (!m_initialized || s_cueFiredCbs.empty()) return;
    PyObject* cueMod = getCueMod(); if (!cueMod) return;
    PyObject* obj = makeCueObject(cueMod, idx); if (!obj) { PyErr_Clear(); return; }
    fireCueCbList(s_cueFiredCbs, obj); Py_DECREF(obj);
}
void ScriptletEngine::fireCueSelectedEvent(int idx) {
    if (!m_initialized || s_cueSelectedCbs.empty()) return;
    PyObject* cueMod = getCueMod(); if (!cueMod) return;
    PyObject* obj = makeCueObject(cueMod, idx); if (!obj) { PyErr_Clear(); return; }
    fireCueCbList(s_cueSelectedCbs, obj); Py_DECREF(obj);
}
void ScriptletEngine::fireCueInsertedEvent(int idx) {
    if (!m_initialized || s_cueInsertedCbs.empty()) return;
    PyObject* cueMod = getCueMod(); if (!cueMod) return;
    PyObject* obj = makeCueObject(cueMod, idx); if (!obj) { PyErr_Clear(); return; }
    fireCueCbList(s_cueInsertedCbs, obj); Py_DECREF(obj);
}
void ScriptletEngine::fireOscEvent(const std::string& path) {
    if (!m_initialized || s_oscEventCbs.empty()) return;
    PyObject* p = PyUnicode_FromString(path.c_str()); if (!p) return;
    auto it = s_oscEventCbs.begin();
    while (it != s_oscEventCbs.end()) {
        PyObject* r = PyObject_CallFunction(it->cb, "O", p);
        if (!r) PyErr_Clear(); else Py_DECREF(r);
        if (it->once) { Py_DECREF(it->cb); it = s_oscEventCbs.erase(it); } else ++it;
    }
    Py_DECREF(p);
}
void ScriptletEngine::fireMidiEvent(int msgType, int ch, int d1, int d2) {
    if (!m_initialized || s_midiEventCbs.empty()) return;
    auto it = s_midiEventCbs.begin();
    while (it != s_midiEventCbs.end()) {
        PyObject* r = PyObject_CallFunction(it->cb, "iiii", msgType, ch, d1, d2);
        if (!r) PyErr_Clear(); else Py_DECREF(r);
        if (it->once) { Py_DECREF(it->cb); it = s_midiEventCbs.erase(it); } else ++it;
    }
}
void ScriptletEngine::fireMusicEvent(int subdivision) {
    if (!m_initialized || s_musicEventCbs.empty()) return;
    auto it = s_musicEventCbs.begin();
    while (it != s_musicEventCbs.end()) {
        if (it->first == subdivision) {
            PyObject* r = PyObject_CallObject(it->second.cb, nullptr);
            if (!r) PyErr_Clear(); else Py_DECREF(r);
            if (it->second.once) { Py_DECREF(it->second.cb); it = s_musicEventCbs.erase(it); continue; }
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------
// Library injection

void ScriptletEngine::setLibrary(const std::vector<std::pair<std::string,std::string>>& modules) {
    m_library = modules;
    if (!m_initialized) return;
    PyObject* sm = PySys_GetObject("modules"); if (!sm) return;
    std::vector<std::string> toRemove;
    PyObject *k = nullptr, *v = nullptr; Py_ssize_t pos = 0;
    while (PyDict_Next(sm, &pos, &k, &v)) {
        const char* ks = PyUnicode_AsUTF8(k);
        if (ks && (std::string(ks) == "mcp.library" ||
                   std::string(ks).rfind("mcp.library.", 0) == 0))
            toRemove.push_back(ks);
    }
    for (const auto& key : toRemove) PyDict_DelItemString(sm, key.c_str());
}

void ScriptletEngine::injectLibrary() {
    if (m_library.empty()) return;
    PyObject* sm = PySys_GetObject("modules"); if (!sm) return;

    PyObject* libMod = PyDict_GetItemString(sm, "mcp.library");
    if (!libMod) {
        libMod = PyImport_AddModule("mcp.library"); if (!libMod) { PyErr_Clear(); return; }
        PyObject* empty = PyList_New(0);
        PyObject_SetAttrString(libMod, "__path__", empty); Py_DECREF(empty);
        PyObject* mcpMod = PyDict_GetItemString(sm, "mcp");
        if (mcpMod) PyObject_SetAttrString(mcpMod, "library", libMod);
    }
    for (const auto& [name, code] : m_library) {
        const std::string full = "mcp.library." + name;
        if (PyDict_GetItemString(sm, full.c_str())) continue;
        PyObject* mod = PyImport_AddModule(full.c_str()); if (!mod) { PyErr_Clear(); continue; }
        PyObject* dict = PyModule_GetDict(mod);
        PyObject* res = PyRun_String(code.c_str(), Py_file_input, dict, dict);
        if (res) { Py_DECREF(res); } else { PyErr_Clear(); }
        PyObject* nm = PyUnicode_FromString(name.c_str());
        PyObject_SetAttr(libMod, nm, mod); Py_DECREF(nm);
    }
}

// ---------------------------------------------------------------------------
// run()

std::string ScriptletEngine::run(const std::string& code) {
    if (code.empty()) return {};

    injectLibrary();

    PyObject* sys    = PyImport_ImportModule("sys");
    PyObject* ioMod  = PyImport_ImportModule("io");
    PyObject* capOut = PyObject_CallMethod(ioMod, "StringIO", nullptr);
    PyObject* capErr = PyObject_CallMethod(ioMod, "StringIO", nullptr);
    PyObject* oldOut = PyObject_GetAttrString(sys, "stdout");
    PyObject* oldErr = PyObject_GetAttrString(sys, "stderr");
    PyObject_SetAttrString(sys, "stdout", capOut);
    PyObject_SetAttrString(sys, "stderr", capErr);

    PyObject* builtins = PyImport_ImportModule("builtins");
    PyObject* globals  = PyDict_New();
    PyDict_SetItemString(globals, "__builtins__", builtins);
    Py_DECREF(builtins);

    PyObject* result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
    Py_DECREF(globals);

    PyObject *ptype = nullptr, *pval = nullptr, *ptb = nullptr;
    if (!result) PyErr_Fetch(&ptype, &pval, &ptb);

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

    std::string combined = capturedOut + capturedErr;

    if (result) {
        Py_DECREF(result);
        if (s_outputCb && !combined.empty()) s_outputCb(combined);
        return {};
    }

    std::string err;
    PyErr_NormalizeException(&ptype, &pval, &ptb);
    PyObject* tbMod = PyImport_ImportModule("traceback");
    if (tbMod) {
        PyObject* fmt = PyObject_GetAttrString(tbMod, "format_exception");
        if (fmt) {
            PyObject* lines = PyObject_CallFunctionObjArgs(
                fmt, ptype ? ptype : Py_None, pval ? pval : Py_None,
                ptb ? ptb : Py_None, nullptr);
            if (lines && PyList_Check(lines))
                for (Py_ssize_t i = 0; i < PyList_Size(lines); ++i)
                    if (const char* s = PyUnicode_AsUTF8(PyList_GetItem(lines, i)))
                        err += s;
            Py_XDECREF(lines); Py_DECREF(fmt);
        }
        Py_DECREF(tbMod);
    }
    if (err.empty() && pval)
        if (PyObject* s = PyObject_Str(pval)) {
            if (const char* cs = PyUnicode_AsUTF8(s)) err = cs;
            Py_DECREF(s);
        }
    Py_XDECREF(ptype); Py_XDECREF(pval); Py_XDECREF(ptb);

    if (err.empty()) err = "Unknown Python error";
    combined += err;
    if (s_outputCb) s_outputCb(combined);
    return err;
}
