#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "ScriptletEngine.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// Static callbacks

static std::function<void()>                   s_goCb;
static std::function<bool(const std::string&)> s_selectCb;
static std::function<void(const std::string&)> s_alertCb;
static std::function<bool(const std::string&)> s_confirmCb;
static std::function<void(const std::string&)> s_outputCb;
static std::function<void()>                   s_panicCb;
static std::function<std::optional<std::string>(const std::string&, const std::string&, const std::string&)> s_fileCb;
static std::function<std::optional<std::string>(const std::string&, const std::string&, const std::string&)> s_inputCb;

// --- mcp.error types (created at interpreter init, kept alive globally) ---
static PyObject* s_errCueNotFound     = nullptr;
static PyObject* s_errCueType         = nullptr;
static PyObject* s_errNoMasterContext = nullptr;
static PyObject* s_errInvalidOp       = nullptr;

// --- mcp.cue active-list action callbacks ---
static std::function<ScriptletCueInfo(int)>              s_cueInfoCb;
static std::function<void(int)>                          s_cueSelectCb;
static std::function<void(int)>                          s_cueGoCb;
static std::function<void(int)>                          s_cueStartCb;
static std::function<void(int, double)>                  s_cueArmCb;
static std::function<void(int)>                          s_cueStopCb;
static std::function<void(int)>                          s_cueDisarmCb;
static std::function<void(int, const std::string&)>      s_cueSetNameCb;

// --- mcp.cue_list.CueList method callbacks (list-ID-aware) ---
static std::function<int(int)>                                                                      s_clCountCb;
static std::function<ScriptletCueInfo(int, int)>                                                    s_clInfoCb;
static std::function<int(int, const std::string&, const std::string&, const std::string&)>         s_clInsertCb;
static std::function<int(int, int, const std::string&, const std::string&, const std::string&)>    s_clInsertAtCb;
static std::function<bool(int, int, int, bool)>                                                     s_clMoveCb;
static std::function<bool(int, int)>                                                                s_clDeleteCb;

// --- mcp.cue_list module-level CRUD ---
static std::function<int(const std::string&)>      s_insertListCb;
static std::function<int(int, const std::string&)> s_insertListAtCb;
static std::function<bool(int)>                    s_deleteListCb;

// --- mcp.mix_console callbacks ---
static std::function<double(const std::string&)>                      s_mixGetParamCb;
static std::function<void(const std::string&, double)>                s_mixSetParamCb;
static std::function<std::vector<ScriptletSnapshotEntry>()>           s_snapshotListCb;
static std::function<bool(int)>                                       s_snapshotLoadCb;
static std::function<bool(int)>                                       s_snapshotStoreCb;
static std::function<bool(int)>                                       s_snapshotDeleteCb;
static std::function<std::vector<std::string>(int)>                   s_snapshotGetScopeCb;
static std::function<void(int, const std::string&, bool)>             s_snapshotSetScopeCb;
static std::function<int()>                                           s_channelCountCb;
static std::function<ScriptletChannelInfo(int)>                       s_channelInfoCb;
static std::function<void(int, const std::string&)>                   s_channelSetNameCb;
static std::function<void(int, float)>                                s_channelSetFaderCb;
static std::function<void(int, bool)>                                 s_channelSetMuteCb;
static std::function<void(int, bool)>                                 s_channelSetPolarityCb;
static std::function<void(int, float)>                                s_channelSetDelayCb;
static std::function<void(int, bool)>                                 s_channelSetPdcCb;
static std::function<float(int, int)>                                 s_channelGetXpointCb;
static std::function<void(int, int, float)>                           s_channelSetXpointCb;
static std::function<void(int, bool)>                                 s_channelLinkCb;
static std::function<int()>                                           s_appendChannelCb;
static std::function<bool(int)>                                       s_removeChannelCb;
static std::function<int(int)>                                        s_pluginSlotCountCb;
static std::function<std::vector<ScriptletPluginParamInfo>(int, int)> s_pluginListParamsCb;
static std::function<float(int, int, const std::string&)>             s_pluginGetParamCb;
static std::function<void(int, int, const std::string&, float)>       s_pluginSetParamCb;
static std::function<bool(int, int, const std::string&)>              s_pluginLoadCb;
static std::function<bool(int, int)>                                  s_pluginUnloadCb;
static std::function<void(int, int)>                                  s_pluginDeactivateScriptletCb;
static std::function<void(int, int)>                                  s_pluginReactivateScriptletCb;
static std::function<int(int)>                                        s_channelSendCountCb;
static std::function<ScriptletSendInfo(int, int)>                     s_sendInfoCb;
static std::function<void(int, int, bool)>                            s_sendSetMuteCb;
static std::function<void(int, int, float)>                           s_sendSetLevelCb;
static std::function<void(int, int, float, float)>                    s_sendSetPanCb;
static std::function<bool(int, int, int)>                             s_sendEngageCb;
static std::function<bool(int, int)>                                  s_sendDisengageCb;

// --- other ---
static std::function<ScriptletMCInfo()>                  s_getMcCb;
static std::function<ScriptletStateInfo()>               s_getStateCb;
static std::function<std::vector<std::pair<int,std::string>>()> s_listInfoCb;
static std::function<int()>                              s_activeListIdCb;

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
    int list_id;  // -1 = active list (s_cueInfoCb); >= 0 = specific list (s_clInfoCb)
} MCPCueObject;

// Guard: Cue cannot be instantiated from Python; only obtainable via C factories.
static PyObject* mcp_cue_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError,
        "Cue cannot be instantiated directly; use mcp.cue.list_cues() or insert_cue()");
    return nullptr;
}

static PyObject* mcp_cue_repr(MCPCueObject* self) {
    ScriptletCueInfo info;
    if (self->list_id >= 0 && s_clInfoCb)
        info = s_clInfoCb(self->list_id, self->index);
    else if (s_cueInfoCb)
        info = s_cueInfoCb(self->index);
    else
        return PyUnicode_FromFormat("<Cue %d>", self->index);
    return PyUnicode_FromFormat("<Cue %s '%s' (%s)>",
                                info.number.c_str(),
                                info.name.c_str(),
                                info.type.c_str());
}

// --- Methods ----------------------------------------------------------------

static inline bool checkStaleCue(MCPCueObject* self) {
    if (self->index >= 0) return false;
    PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
    PyErr_SetString(exc, "cue is stale (already deleted)");
    return true;
}

static PyObject* mcp_cue_select(MCPCueObject* self, PyObject*) {
    if (checkStaleCue(self)) return nullptr;
    if (s_cueSelectCb) s_cueSelectCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_go(MCPCueObject* self, PyObject*) {
    if (checkStaleCue(self)) return nullptr;
    if (s_cueGoCb) s_cueGoCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_start(MCPCueObject* self, PyObject*) {
    if (checkStaleCue(self)) return nullptr;
    if (s_cueStartCb) s_cueStartCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_arm(MCPCueObject* self, PyObject* args, PyObject* kwargs) {
    if (checkStaleCue(self)) return nullptr;
    static const char* kwlist[] = {"time", nullptr};
    PyObject* timeObj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O",
            const_cast<char**>(kwlist), &timeObj)) return nullptr;

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
    if (checkStaleCue(self)) return nullptr;
    if (s_cueStopCb) s_cueStopCb(self->index);
    Py_RETURN_NONE;
}
static PyObject* mcp_cue_disarm(MCPCueObject* self, PyObject*) {
    if (checkStaleCue(self)) return nullptr;
    if (s_cueDisarmCb) s_cueDisarmCb(self->index);
    Py_RETURN_NONE;
}

static PyMethodDef kCueMethods[] = {
    {"select",  (PyCFunction)mcp_cue_select, METH_NOARGS,  "select()  — set as selected cue (cursor only)"},
    {"go",      (PyCFunction)mcp_cue_go,     METH_NOARGS,  "go()  — select this cue and fire it"},
    {"start",   (PyCFunction)mcp_cue_start,  METH_NOARGS,  "start()  — fire without changing selection"},
    {"arm",     (PyCFunction)mcp_cue_arm,    METH_VARARGS | METH_KEYWORDS, "arm(time: Time = None)  — pre-buffer from position"},
    {"stop",    (PyCFunction)mcp_cue_stop,   METH_NOARGS,  "stop()  — stop all active voices"},
    {"disarm",  (PyCFunction)mcp_cue_disarm, METH_NOARGS,  "disarm()  — release pre-buffered audio"},
    {nullptr}
};

// --- Properties (getset) ----------------------------------------------------

static inline bool getInfo(MCPCueObject* self, ScriptletCueInfo& out) {
    if (self->list_id >= 0) {
        if (!s_clInfoCb) { PyErr_SetString(PyExc_RuntimeError, "no cue list info callback"); return false; }
        out = s_clInfoCb(self->list_id, self->index);
        return true;
    }
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
    if (self->index < 0) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "cue is stale (already deleted)"); return -1;
    }
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
static PyObject* mcp_time_to_timecode(MCPTimeObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"fps", nullptr};
    double fps = 25.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|d",
            const_cast<char**>(kwlist), &fps)) return nullptr;
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
static PyObject* mcp_time_from_timecode (PyObject*, PyObject*, PyObject*);
static PyObject* mcp_time_from_bar_beat (PyObject*, PyObject*);

static PyMethodDef kTimeMethods[] = {
    // Instance methods
    {"to_seconds",    (PyCFunction)mcp_time_to_seconds,    METH_NOARGS,                  "to_seconds() → float"},
    {"to_samples",    (PyCFunction)mcp_time_to_samples,    METH_NOARGS,                  "to_samples() → int"},
    {"to_min_sec",    (PyCFunction)mcp_time_to_min_sec,    METH_NOARGS,                  "to_min_sec() → (int, float)"},
    {"to_timecode",   (PyCFunction)mcp_time_to_timecode,   METH_VARARGS | METH_KEYWORDS, "to_timecode(fps=25) → 'HH:MM:SS:FF'"},
    // Static factory methods (PyType_Ready wraps these in staticmethod descriptors)
    {"from_sample",   (PyCFunction)mcp_time_from_sample,   METH_VARARGS | METH_STATIC,   "from_sample(n) → Time"},
    {"from_min_sec",  (PyCFunction)mcp_time_from_min_sec,  METH_VARARGS | METH_STATIC,   "from_min_sec(min, sec) → Time"},
    {"from_timecode", (PyCFunction)mcp_time_from_timecode, METH_VARARGS | METH_KEYWORDS | METH_STATIC, "from_timecode(tc, fps=25) → Time"},
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
static PyObject* mcp_time_from_timecode(PyObject*, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"timecode", "fps", nullptr};
    const char* tc = nullptr; double fps = 25.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|d",
            const_cast<char**>(kwlist), &tc, &fps)) return nullptr;
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
    if (t == "audio")          return "AudioCue";
    if (t == "start")          return "StartCue";
    if (t == "stop")           return "StopCue";
    if (t == "arm")            return "ArmCue";
    if (t == "fade")           return "FadeCue";
    if (t == "devamp")         return "DevampCue";
    if (t == "goto")           return "GotoCue";
    if (t == "memo")           return "MemoCue";
    if (t == "scriptlet")      return "ScriptletCue";
    if (t == "network")        return "NetworkCue";
    if (t == "midi")           return "MidiCue";
    if (t == "timecode")       return "TimecodeCue";
    if (t == "marker")         return "MarkerCue";
    if (t == "group")          return "GroupCue";
    if (t == "snapshot")       return "SnapshotCue";
    if (t == "automation")     return "AutomationCue";
    if (t == "music_context")  return "MCCue";
    if (t == "deactivate")     return "DeactivateCue";
    if (t == "reactivate")     return "ReactivateCue";
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
    obj->index   = idx;
    obj->list_id = -1;
    return (PyObject*)obj;
}

// ---------------------------------------------------------------------------
// mcp.cue module functions

static PyObject* mcp_cue_get_selected(PyObject* module, PyObject*) {
    if (!s_getStateCb) Py_RETURN_NONE;
    const auto state = s_getStateCb();
    if (state.selectedCue < 0) Py_RETURN_NONE;
    return makeCueObject(module, state.selectedCue);
}

static PyObject* mcp_cue_get_current(PyObject* module, PyObject*) {
    // "current" == cursor position == selected
    return mcp_cue_get_selected(module, nullptr);
}

static PyObject* mcp_cue_get_active(PyObject* module, PyObject*) {
    if (!s_getStateCb) return PyList_New(0);
    const auto state = s_getStateCb();
    PyObject* result = PyList_New(static_cast<Py_ssize_t>(state.runningCues.size()));
    if (!result) return nullptr;
    for (size_t i = 0; i < state.runningCues.size(); ++i) {
        PyObject* obj = makeCueObject(module, state.runningCues[i]);
        if (!obj) { PyErr_Clear(); obj = Py_None; Py_INCREF(obj); }
        PyList_SET_ITEM(result, static_cast<Py_ssize_t>(i), obj);
    }
    return result;
}

static PyMethodDef kCueModuleMethods[] = {
    {"get_selected_cue", mcp_cue_get_selected, METH_NOARGS, "get_selected_cue() → Cue | None"},
    {"get_current_cue",  mcp_cue_get_current,  METH_NOARGS, "get_current_cue() → Cue | None"},
    {"get_active_cues",  mcp_cue_get_active,   METH_NOARGS, "get_active_cues() → [Cue]"},
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
AudioCue      = type('AudioCue',      (Cue,), {'__doc__': 'Audio playback cue'})
StartCue      = type('StartCue',      (Cue,), {'__doc__': 'Start/trigger another cue'})
StopCue       = type('StopCue',       (Cue,), {'__doc__': 'Stop another cue'})
ArmCue        = type('ArmCue',        (Cue,), {'__doc__': 'Pre-buffer another cue'})
FadeCue       = type('FadeCue',       (Cue,), {'__doc__': 'Fade another cue'})
DevampCue     = type('DevampCue',     (Cue,), {'__doc__': 'Exit-vamp / devamp cue'})
GotoCue       = type('GotoCue',       (Cue,), {'__doc__': 'Jump to a cue number'})
MemoCue       = type('MemoCue',       (Cue,), {'__doc__': 'Memo / label cue'})
ScriptletCue  = type('ScriptletCue',  (Cue,), {'__doc__': 'Python scriptlet cue'})
NetworkCue    = type('NetworkCue',    (Cue,), {'__doc__': 'Network output cue'})
MidiCue       = type('MidiCue',       (Cue,), {'__doc__': 'MIDI output cue'})
TimecodeCue   = type('TimecodeCue',   (Cue,), {'__doc__': 'LTC/MTC timecode cue'})
MarkerCue     = type('MarkerCue',     (Cue,), {'__doc__': 'Start from a time marker'})
GroupCue      = type('GroupCue',      (Cue,), {'__doc__': 'Group / timeline cue'})
SnapshotCue   = type('SnapshotCue',   (Cue,), {'__doc__': 'Recall a mix snapshot'})
AutomationCue = type('AutomationCue', (Cue,), {'__doc__': 'Drive a parameter curve'})
MCCue         = type('MCCue',         (Cue,), {'__doc__': 'Music context cue'})
DeactivateCue = type('DeactivateCue', (Cue,), {'__doc__': 'Deactivate a plugin slot'})
ReactivateCue = type('ReactivateCue', (Cue,), {'__doc__': 'Reactivate a plugin slot'})
)py";

    PyObject* modDict = PyModule_GetDict(mod);
    PyObject* res = PyRun_String(kSubclasses, Py_file_input, modDict, modDict);
    if (res) { Py_DECREF(res); } else { PyErr_Clear(); }

    return mod;
}

// ---------------------------------------------------------------------------
// Valid cue type strings accepted by insert_cue / insert_cue_at.
static bool isValidCueType(const char* t) {
    static const char* const kTypes[] = {
        "audio", "memo", "group", "start", "stop", "arm", "devamp",
        "fade", "timecode", "midi", "network", "goto", "marker", "scriptlet",
        "snapshot", "automation", "deactivate", "reactivate",
        nullptr
    };
    if (!t || !*t) return false;
    for (int i = 0; kTypes[i]; ++i)
        if (strcmp(t, kTypes[i]) == 0) return true;
    return false;
}

// mcp.cue_list — CueList extension type

typedef struct {
    PyObject_HEAD
    int list_id;
} MCPCueListObject;

static PyObject* mcp_cuelist_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError,
        "CueList cannot be instantiated directly; use mcp.cue_list.get_active_cue_list()");
    return nullptr;
}

static PyObject* mcp_cuelist_repr(MCPCueListObject* self) {
    return PyUnicode_FromFormat("<CueList id=%d>", self->list_id);
}

static PyObject* mcp_cuelist_get_id(MCPCueListObject* self, void*) {
    return PyLong_FromLong(self->list_id);
}

// Helper: build a Cue object whose type is resolved via s_clInfoCb (list-aware).
static PyObject* makeCueObjectForList(PyObject* cueMod, int list_id, int idx) {
    PyTypeObject* type = &MCPCueType;
    if (cueMod && s_clInfoCb) {
        const auto info = s_clInfoCb(list_id, idx);
        const char* clsName = classForType(info.type);
        PyObject* cls = PyObject_GetAttrString(cueMod, clsName);
        if (cls) { type = (PyTypeObject*)cls; Py_DECREF(cls); }
        else PyErr_Clear();
    }
    MCPCueObject* obj = (MCPCueObject*)type->tp_alloc(type, 0);
    if (!obj) return nullptr;
    obj->index   = idx;
    obj->list_id = list_id;
    return (PyObject*)obj;
}

// Forward-declare CueList methods so kCueListMethods can reference them before MCPCueListType.
static PyObject* mcp_cuelist_list_cues    (MCPCueListObject*, PyObject*);
static PyObject* mcp_cuelist_insert_cue   (MCPCueListObject*, PyObject*, PyObject*);
static PyObject* mcp_cuelist_insert_cue_at(MCPCueListObject*, PyObject*, PyObject*);
static PyObject* mcp_cuelist_move_cue_at  (MCPCueListObject*, PyObject*, PyObject*);
static PyObject* mcp_cuelist_delete_cue   (MCPCueListObject*, PyObject*);

static PyMethodDef kCueListMethods[] = {
    {"list_cues",     (PyCFunction)mcp_cuelist_list_cues,     METH_NOARGS,                    "list_cues() → [Cue]"},
    {"insert_cue",    (PyCFunction)mcp_cuelist_insert_cue,    METH_VARARGS | METH_KEYWORDS,   "insert_cue(type, cuenumber='', cuename='') → Cue"},
    {"insert_cue_at", (PyCFunction)mcp_cuelist_insert_cue_at, METH_VARARGS | METH_KEYWORDS,   "insert_cue_at(ref, type, cuenumber='', cuename='') → Cue"},
    {"move_cue_at",   (PyCFunction)mcp_cuelist_move_cue_at,   METH_VARARGS | METH_KEYWORDS,   "move_cue_at(ref, cue, to_group=False)"},
    {"delete_cue",    (PyCFunction)mcp_cuelist_delete_cue,    METH_O,                         "delete_cue(cue)"},
    {nullptr}
};

static PyObject* mcp_cuelist_get_name(MCPCueListObject* self, void*) {
    if (s_listInfoCb) {
        for (const auto& [id, name] : s_listInfoCb())
            if (id == self->list_id)
                return PyUnicode_FromString(name.c_str());
    }
    return PyUnicode_FromString("");
}

static PyGetSetDef kCueListGetSet[] = {
    {const_cast<char*>("id"),   (getter_func)mcp_cuelist_get_id,   nullptr, const_cast<char*>("numeric list ID"), nullptr},
    {const_cast<char*>("name"), (getter_func)mcp_cuelist_get_name, nullptr, const_cast<char*>("list name"),       nullptr},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyTypeObject MCPCueListType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.cue_list.CueList",
    sizeof(MCPCueListObject),
    0, nullptr,
    0, nullptr, nullptr, nullptr,
    (reprfunc)mcp_cuelist_repr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "CueList — handle to a cue list (obtain via mcp.cue_list.get_active_cue_list())",
    nullptr, nullptr, nullptr,
    0, nullptr, nullptr,
    kCueListMethods,
    nullptr,
    kCueListGetSet,
    nullptr, nullptr, nullptr, nullptr, 0,
    nullptr, nullptr,
    mcp_cuelist_new_guard,
};
#pragma GCC diagnostic pop

static PyObject* makeCueListObject(int list_id) {
    MCPCueListObject* obj = (MCPCueListObject*)MCPCueListType.tp_alloc(&MCPCueListType, 0);
    if (!obj) return nullptr;
    obj->list_id = list_id;
    return (PyObject*)obj;
}

// CueList method definitions.

static PyObject* mcp_cuelist_list_cues(MCPCueListObject* self, PyObject*) {
    if (self->list_id < 0) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has been deleted"); return nullptr;
    }
    if (!s_clCountCb || !s_clInfoCb) return PyList_New(0);
    const int count = s_clCountCb(self->list_id);
    if (count < 0) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has been deleted"); return nullptr;
    }
    PyObject* result = PyList_New(count);
    if (!result) return nullptr;
    PyObject* cueMod = getCueMod();
    for (int i = 0; i < count; ++i) {
        PyObject* obj = makeCueObjectForList(cueMod, self->list_id, i);
        if (!obj) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, i, obj);
    }
    return result;
}

static PyObject* mcp_cuelist_insert_cue(MCPCueListObject* self, PyObject* args, PyObject* kwargs) {
    if (self->list_id < 0) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has been deleted"); return nullptr;
    }
    static const char* kwlist[] = {"type", "cuenumber", "cuename", nullptr};
    const char* type   = nullptr;
    const char* number = "";
    const char* name   = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ss",
            const_cast<char**>(kwlist), &type, &number, &name)) return nullptr;
    if (!isValidCueType(type)) {
        PyObject* exc = s_errCueType ? s_errCueType : PyExc_ValueError;
        PyErr_Format(exc, "unknown cue type: '%s'", type ? type : ""); return nullptr;
    }
    if (!s_clInsertCb) { PyErr_SetString(PyExc_RuntimeError, "no insert callback"); return nullptr; }
    const int newIdx = s_clInsertCb(self->list_id,
                                     type   ? type   : "",
                                     number ? number : "",
                                     name   ? name   : "");
    if (newIdx < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue failed"); return nullptr; }
    return makeCueObjectForList(getCueMod(), self->list_id, newIdx);
}

static PyObject* mcp_cuelist_insert_cue_at(MCPCueListObject* self, PyObject* args, PyObject* kwargs) {
    if (self->list_id < 0) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has been deleted"); return nullptr;
    }
    static const char* kwlist[] = {"ref", "type", "cuenumber", "cuename", nullptr};
    PyObject*   refObj = nullptr;
    const char* type   = nullptr;
    const char* number = "";
    const char* name   = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os|ss",
            const_cast<char**>(kwlist), &refObj, &type, &number, &name)) return nullptr;
    if (!PyObject_IsInstance(refObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "ref must be a Cue"); return nullptr;
    }
    const int refIdx = ((MCPCueObject*)refObj)->index;
    if (refIdx < 0) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "ref cue is stale (already deleted)"); return nullptr;
    }
    if (!isValidCueType(type)) {
        PyObject* exc = s_errCueType ? s_errCueType : PyExc_ValueError;
        PyErr_Format(exc, "unknown cue type: '%s'", type ? type : ""); return nullptr;
    }
    if (!s_clInsertAtCb) { PyErr_SetString(PyExc_RuntimeError, "no insert_at callback"); return nullptr; }
    const int newIdx = s_clInsertAtCb(self->list_id, refIdx,
                                       type   ? type   : "",
                                       number ? number : "",
                                       name   ? name   : "");
    if (newIdx < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue_at failed"); return nullptr; }
    return makeCueObjectForList(getCueMod(), self->list_id, newIdx);
}

static PyObject* mcp_cuelist_move_cue_at(MCPCueListObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"ref", "cue", "to_group", nullptr};
    PyObject* refObj  = nullptr;
    PyObject* cueObj  = nullptr;
    int       toGroup = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|p",
            const_cast<char**>(kwlist), &refObj, &cueObj, &toGroup)) return nullptr;
    if (!PyObject_IsInstance(refObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "ref must be a Cue"); return nullptr;
    }
    if (!PyObject_IsInstance(cueObj, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "cue must be a Cue"); return nullptr;
    }
    if (!s_clMoveCb) { PyErr_SetString(PyExc_RuntimeError, "no move callback"); return nullptr; }
    const int refIdx = ((MCPCueObject*)refObj)->index;
    const int cueIdx = ((MCPCueObject*)cueObj)->index;
    if (refIdx < 0) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "ref cue is stale (already deleted)"); return nullptr;
    }
    if (cueIdx < 0) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "cue is stale (already deleted)"); return nullptr;
    }
    if (!s_clMoveCb(self->list_id, refIdx, cueIdx, toGroup != 0)) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "move_cue_at: cue index out of range"); return nullptr;
    }
    Py_RETURN_NONE;
}

static PyObject* mcp_cuelist_delete_cue(MCPCueListObject* self, PyObject* arg) {
    if (!PyObject_IsInstance(arg, (PyObject*)&MCPCueType)) {
        PyErr_SetString(PyExc_TypeError, "arg must be a Cue"); return nullptr;
    }
    const int idx = ((MCPCueObject*)arg)->index;
    if (idx < 0) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "cue is stale (already deleted)"); return nullptr;
    }
    if (!s_clDeleteCb) { PyErr_SetString(PyExc_RuntimeError, "no delete callback"); return nullptr; }
    if (!s_clDeleteCb(self->list_id, idx)) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_SetString(exc, "delete_cue: cue index out of range"); return nullptr;
    }
    ((MCPCueObject*)arg)->index = -1;
    Py_RETURN_NONE;
}

// mcp.cue_list module-level functions.

static PyObject* py_cue_list_get_active(PyObject*, PyObject*) {
    if (!s_activeListIdCb) { PyErr_SetString(PyExc_RuntimeError, "no active list callback"); return nullptr; }
    return makeCueListObject(s_activeListIdCb());
}

static PyObject* py_cue_list_list(PyObject*, PyObject*) {
    if (!s_listInfoCb) return PyList_New(0);
    const auto lists = s_listInfoCb();
    PyObject* result = PyList_New(static_cast<Py_ssize_t>(lists.size()));
    if (!result) return nullptr;
    for (size_t i = 0; i < lists.size(); ++i) {
        PyObject* obj = makeCueListObject(lists[i].first);
        if (!obj) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, static_cast<Py_ssize_t>(i), obj);
    }
    return result;
}

static PyObject* py_cue_list_insert(PyObject*, PyObject* args) {
    const char* name = "";
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    if (!s_insertListCb) { PyErr_SetString(PyExc_RuntimeError, "no insert list callback"); return nullptr; }
    const int id = s_insertListCb(name ? name : "");
    if (id < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue_list failed"); return nullptr; }
    return makeCueListObject(id);
}

static PyObject* py_cue_list_insert_at(PyObject*, PyObject* args) {
    PyObject*   refObj = nullptr;
    const char* name   = "";
    if (!PyArg_ParseTuple(args, "Os", &refObj, &name)) return nullptr;
    if (!PyObject_IsInstance(refObj, (PyObject*)&MCPCueListType)) {
        PyErr_SetString(PyExc_TypeError, "ref must be a CueList"); return nullptr;
    }
    const int refId = ((MCPCueListObject*)refObj)->list_id;
    if (!s_insertListAtCb) { PyErr_SetString(PyExc_RuntimeError, "no insert_at list callback"); return nullptr; }
    const int id = s_insertListAtCb(refId, name ? name : "");
    if (id < 0) { PyErr_SetString(PyExc_RuntimeError, "insert_cue_list_at failed"); return nullptr; }
    return makeCueListObject(id);
}

static PyObject* py_cue_list_delete(PyObject*, PyObject* arg) {
    if (!PyObject_IsInstance(arg, (PyObject*)&MCPCueListType)) {
        PyErr_SetString(PyExc_TypeError, "arg must be a CueList"); return nullptr;
    }
    const int id = ((MCPCueListObject*)arg)->list_id;
    if (id < 0) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has already been deleted"); return nullptr;
    }
    if (!s_deleteListCb) { PyErr_SetString(PyExc_RuntimeError, "no delete list callback"); return nullptr; }
    if (!s_deleteListCb(id)) {
        PyObject* exc = s_errInvalidOp ? s_errInvalidOp : PyExc_RuntimeError;
        PyErr_SetString(exc, "cue list has already been deleted"); return nullptr;
    }
    ((MCPCueListObject*)arg)->list_id = -1;
    Py_RETURN_NONE;
}

static PyMethodDef kCueListModuleMethods[] = {
    {"get_active_cue_list", py_cue_list_get_active,  METH_NOARGS,  "get_active_cue_list() → CueList"},
    {"list_cue_lists",      py_cue_list_list,         METH_NOARGS,  "list_cue_lists() → [CueList]"},
    {"insert_cue_list",     py_cue_list_insert,       METH_VARARGS, "insert_cue_list(name) → CueList"},
    {"insert_cue_list_at",  py_cue_list_insert_at,    METH_VARARGS, "insert_cue_list_at(ref, name) → CueList"},
    {"delete_cue_list",     py_cue_list_delete,       METH_O,       "delete_cue_list(cue_list)"},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kCueListModuleDef = {
    PyModuleDef_HEAD_INIT, "mcp.cue_list", "mcp cue-list API", -1, kCueListModuleMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp_cue_list() {
    if (PyType_Ready(&MCPCueListType) < 0) return nullptr;
    PyObject* mod = PyModule_Create(&kCueListModuleDef);
    if (!mod) return nullptr;
    Py_INCREF(&MCPCueListType);
    PyModule_AddObject(mod, "CueList", (PyObject*)&MCPCueListType);
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

static PyObject* eventUnsubscribe(std::vector<EventCb>& vec, PyObject* arg) {
    if (!PyCallable_Check(arg)) { PyErr_SetString(PyExc_TypeError, "callback must be callable"); return nullptr; }
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        if (it->cb == arg) {
            Py_DECREF(it->cb);
            vec.erase(it);
            break;
        }
    }
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

static PyObject* mcp_event_unsubscribe_cue_fired   (PyObject*, PyObject* a) { return eventUnsubscribe(s_cueFiredCbs,    a); }
static PyObject* mcp_event_unsubscribe_cue_selected(PyObject*, PyObject* a) { return eventUnsubscribe(s_cueSelectedCbs, a); }
static PyObject* mcp_event_unsubscribe_cue_inserted(PyObject*, PyObject* a) { return eventUnsubscribe(s_cueInsertedCbs, a); }
static PyObject* mcp_event_unsubscribe_osc_event   (PyObject*, PyObject* a) { return eventUnsubscribe(s_oscEventCbs,    a); }
static PyObject* mcp_event_unsubscribe_midi_event  (PyObject*, PyObject* a) { return eventUnsubscribe(s_midiEventCbs,   a); }
static PyObject* mcp_event_unsubscribe_music_event (PyObject*, PyObject* args) {
    const char* quant = nullptr;
    PyObject*   cb    = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &quant, &cb)) return nullptr;
    if (!PyCallable_Check(cb)) { PyErr_SetString(PyExc_TypeError, "callback must be callable"); return nullptr; }
    for (auto it = s_musicEventCbs.begin(); it != s_musicEventCbs.end(); ++it) {
        if (it->second.cb == cb) {
            Py_DECREF(it->second.cb);
            s_musicEventCbs.erase(it);
            break;
        }
    }
    Py_RETURN_NONE;
}

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
    {"on_music_event",             mcp_event_on_music_event,             METH_VARARGS, "on_music_event(quantization, cb)"},
    {"once_music_event",           mcp_event_once_music_event,           METH_VARARGS, "once_music_event(quantization, cb)"},
    {"unsubscribe_cue_fired",      mcp_event_unsubscribe_cue_fired,      METH_O,       "unsubscribe_cue_fired(cb)"},
    {"unsubscribe_cue_selected",   mcp_event_unsubscribe_cue_selected,   METH_O,       "unsubscribe_cue_selected(cb)"},
    {"unsubscribe_cue_inserted",   mcp_event_unsubscribe_cue_inserted,   METH_O,       "unsubscribe_cue_inserted(cb)"},
    {"unsubscribe_osc_event",      mcp_event_unsubscribe_osc_event,      METH_O,       "unsubscribe_osc_event(cb)"},
    {"unsubscribe_midi_event",     mcp_event_unsubscribe_midi_event,     METH_O,       "unsubscribe_midi_event(cb)"},
    {"unsubscribe_music_event",    mcp_event_unsubscribe_music_event,    METH_VARARGS, "unsubscribe_music_event(quantization, cb)"},
    {"clear_all",                  mcp_event_clear_all,                  METH_NOARGS,  "clear_all()"},
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
// mcp.mix_console module

// Forward declarations for cross-type references
extern PyTypeObject MCPPluginSlotType;
extern PyTypeObject MCPSendSlotType;

typedef struct { PyObject_HEAD int ch; int slot; } MCPPluginSlotObject;
typedef struct { PyObject_HEAD int ch; } MCPChannelObject;

static PyObject* mcp_channel_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "Channel cannot be instantiated directly; use mcp.mix_console.get_channel()");
    return nullptr;
}
static PyObject* mcp_channel_repr(MCPChannelObject* self) { return PyUnicode_FromFormat("<Channel %d>", self->ch); }

static PyObject* mcp_channel_get_name(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) Py_RETURN_NONE;
    return PyUnicode_FromString(s_channelInfoCb(self->ch).name.c_str());
}
static int mcp_channel_set_name(MCPChannelObject* self, PyObject* val, void*) {
    if (!val || !PyUnicode_Check(val)) { PyErr_SetString(PyExc_TypeError, "name must be a str"); return -1; }
    const char* s = PyUnicode_AsUTF8(val); if (!s) return -1;
    if (s_channelSetNameCb) s_channelSetNameCb(self->ch, s);
    return 0;
}
static PyObject* mcp_channel_get_fader(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_channelInfoCb(self->ch).fader);
}
static int mcp_channel_set_fader(MCPChannelObject* self, PyObject* val, void*) {
    PyObject* f = PyNumber_Float(val); if (!f) return -1;
    const double v = PyFloat_AsDouble(f); Py_DECREF(f);
    if (PyErr_Occurred()) return -1;
    if (s_channelSetFaderCb) s_channelSetFaderCb(self->ch, (float)v);
    return 0;
}
static PyObject* mcp_channel_get_mute(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) Py_RETURN_FALSE;
    return PyBool_FromLong(s_channelInfoCb(self->ch).mute);
}
static int mcp_channel_set_mute(MCPChannelObject* self, PyObject* val, void*) {
    if (s_channelSetMuteCb) s_channelSetMuteCb(self->ch, PyObject_IsTrue(val) != 0);
    return 0;
}
static PyObject* mcp_channel_get_polarity(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) Py_RETURN_FALSE;
    return PyBool_FromLong(s_channelInfoCb(self->ch).polarity);
}
static int mcp_channel_set_polarity(MCPChannelObject* self, PyObject* val, void*) {
    if (s_channelSetPolarityCb) s_channelSetPolarityCb(self->ch, PyObject_IsTrue(val) != 0);
    return 0;
}
static PyObject* mcp_channel_get_delay(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_channelInfoCb(self->ch).delay);
}
static int mcp_channel_set_delay(MCPChannelObject* self, PyObject* val, void*) {
    PyObject* f = PyNumber_Float(val); if (!f) return -1;
    const double v = PyFloat_AsDouble(f); Py_DECREF(f);
    if (PyErr_Occurred()) return -1;
    if (s_channelSetDelayCb) s_channelSetDelayCb(self->ch, (float)v);
    return 0;
}
static PyObject* mcp_channel_get_pdc_isolation(MCPChannelObject* self, void*) {
    if (!s_channelInfoCb) Py_RETURN_FALSE;
    return PyBool_FromLong(s_channelInfoCb(self->ch).pdcIsolation);
}
static int mcp_channel_set_pdc_isolation(MCPChannelObject* self, PyObject* val, void*) {
    if (s_channelSetPdcCb) s_channelSetPdcCb(self->ch, PyObject_IsTrue(val) != 0);
    return 0;
}

static PyObject* mcp_channel_get_ch_idx(MCPChannelObject* self, void*) { return PyLong_FromLong(self->ch); }

static PyGetSetDef kChannelGetSet[] = {
    RO("ch",            mcp_channel_get_ch_idx,                                        "channel index (0-based)"),
    RW("name",          mcp_channel_get_name,          mcp_channel_set_name,          "channel name"),
    RW("fader",         mcp_channel_get_fader,         mcp_channel_set_fader,         "fader level in dB"),
    RW("mute",          mcp_channel_get_mute,          mcp_channel_set_mute,          "mute state"),
    RW("polarity",      mcp_channel_get_polarity,      mcp_channel_set_polarity,      "phase invert"),
    RW("delay",         mcp_channel_get_delay,         mcp_channel_set_delay,         "delay in ms"),
    RW("pdc_isolation", mcp_channel_get_pdc_isolation, mcp_channel_set_pdc_isolation, "PDC isolation"),
    {nullptr}
};

static PyObject* mcp_channel_get_link_state(MCPChannelObject* self, PyObject*) {
    if (!s_channelInfoCb) return PyUnicode_FromString("mono");
    return PyUnicode_FromString(s_channelInfoCb(self->ch).linkState.c_str());
}
static PyObject* mcp_channel_link(MCPChannelObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"link", nullptr};
    int linked = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|p", const_cast<char**>(kwlist), &linked)) return nullptr;
    if (s_channelLinkCb) s_channelLinkCb(self->ch, linked != 0);
    Py_RETURN_NONE;
}
static PyObject* mcp_channel_get_crosspoint(MCPChannelObject* self, PyObject* args) {
    int out = 0; if (!PyArg_ParseTuple(args, "i", &out)) return nullptr;
    if (!s_channelGetXpointCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_channelGetXpointCb(self->ch, out));
}
static PyObject* mcp_channel_set_crosspoint(MCPChannelObject* self, PyObject* args) {
    int out = 0; double db = 0.0;
    if (!PyArg_ParseTuple(args, "id", &out, &db)) return nullptr;
    if (s_channelSetXpointCb) s_channelSetXpointCb(self->ch, out, (float)db);
    Py_RETURN_NONE;
}
static PyObject* mcp_channel_get_plugin_slot(MCPChannelObject* self, PyObject* args);  // defined after MCPPluginSlotType
static PyObject* mcp_channel_get_send_slot  (MCPChannelObject* self, PyObject* args);  // defined after MCPSendSlotType

static PyMethodDef kChannelMethods[] = {
    {"get_link_state",  (PyCFunction)mcp_channel_get_link_state,  METH_NOARGS,                    "get_link_state() → str"},
    {"link",            (PyCFunction)mcp_channel_link,             METH_VARARGS | METH_KEYWORDS,   "link(link=True)"},
    {"get_crosspoint",  (PyCFunction)mcp_channel_get_crosspoint,  METH_VARARGS,                   "get_crosspoint(out) → float"},
    {"set_crosspoint",  (PyCFunction)mcp_channel_set_crosspoint,  METH_VARARGS,                   "set_crosspoint(out, db)"},
    {"get_plugin_slot", (PyCFunction)mcp_channel_get_plugin_slot, METH_VARARGS,                   "get_plugin_slot(id) → PluginSlot"},
    {"get_send_slot",   (PyCFunction)mcp_channel_get_send_slot,   METH_VARARGS,                   "get_send_slot(id) → SendSlot"},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyTypeObject MCPChannelType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.mix_console.Channel", sizeof(MCPChannelObject), 0, nullptr,
    0, nullptr, nullptr, nullptr, (reprfunc)mcp_channel_repr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT, "Channel — mix console channel handle",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    kChannelMethods, nullptr, kChannelGetSet,
    nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    mcp_channel_new_guard,
};
#pragma GCC diagnostic pop

// --- MCPSendSlotObject ---

typedef struct { PyObject_HEAD int ch; int slot; } MCPSendSlotObject;

static PyObject* mcp_sendslot_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "SendSlot cannot be instantiated directly");
    return nullptr;
}
static PyObject* mcp_sendslot_repr(MCPSendSlotObject* self) {
    return PyUnicode_FromFormat("<SendSlot ch=%d slot=%d>", self->ch, self->slot);
}
static PyObject* mcp_sendslot_get_mute(MCPSendSlotObject* self, void*) {
    if (!s_sendInfoCb) Py_RETURN_FALSE;
    return PyBool_FromLong(s_sendInfoCb(self->ch, self->slot).mute);
}
static int mcp_sendslot_set_mute(MCPSendSlotObject* self, PyObject* val, void*) {
    if (s_sendSetMuteCb) s_sendSetMuteCb(self->ch, self->slot, PyObject_IsTrue(val) != 0);
    return 0;
}
static PyObject* mcp_sendslot_get_level(MCPSendSlotObject* self, void*) {
    if (!s_sendInfoCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_sendInfoCb(self->ch, self->slot).level);
}
static int mcp_sendslot_set_level(MCPSendSlotObject* self, PyObject* val, void*) {
    PyObject* f = PyNumber_Float(val); if (!f) return -1;
    const double v = PyFloat_AsDouble(f); Py_DECREF(f);
    if (PyErr_Occurred()) return -1;
    if (s_sendSetLevelCb) s_sendSetLevelCb(self->ch, self->slot, (float)v);
    return 0;
}
static PyObject* mcp_sendslot_get_pan(MCPSendSlotObject* self, void*) {
    if (!s_sendInfoCb) return Py_BuildValue("(ff)", 0.0f, 0.0f);
    const auto info = s_sendInfoCb(self->ch, self->slot);
    return Py_BuildValue("(ff)", info.panL, info.panR);
}
static PyObject* mcp_sendslot_get_ch_idx  (MCPSendSlotObject* self, void*) { return PyLong_FromLong(self->ch);   }
static PyObject* mcp_sendslot_get_slot_idx(MCPSendSlotObject* self, void*) { return PyLong_FromLong(self->slot); }

static PyGetSetDef kSendSlotGetSet[] = {
    RO("ch",    mcp_sendslot_get_ch_idx,                        "channel index (0-based)"),
    RO("slot",  mcp_sendslot_get_slot_idx,                      "slot index (0-based)"),
    RW("mute",  mcp_sendslot_get_mute,  mcp_sendslot_set_mute,  "mute state"),
    RW("level", mcp_sendslot_get_level, mcp_sendslot_set_level, "send level dB"),
    RO("pan",   mcp_sendslot_get_pan,                           "pan as (panL, panR)"),
    {nullptr}
};
static PyObject* mcp_sendslot_set_pan_method(MCPSendSlotObject* self, PyObject* args) {
    double panL = 0.0, panR = 0.0;
    if (!PyArg_ParseTuple(args, "dd", &panL, &panR)) return nullptr;
    if (s_sendSetPanCb) s_sendSetPanCb(self->ch, self->slot, (float)panL, (float)panR);
    Py_RETURN_NONE;
}
static PyObject* mcp_sendslot_engage(MCPSendSlotObject* self, PyObject* arg) {
    int dstCh = -1;
    if (PyLong_Check(arg)) {
        dstCh = (int)PyLong_AsLong(arg);
    } else if (PyObject_IsInstance(arg, (PyObject*)&MCPChannelType)) {
        dstCh = ((MCPChannelObject*)arg)->ch;
    } else {
        PyErr_SetString(PyExc_TypeError, "target_channel must be int or Channel"); return nullptr;
    }
    if (!s_sendEngageCb) { PyErr_SetString(PyExc_RuntimeError, "no send engage callback"); return nullptr; }
    if (!s_sendEngageCb(self->ch, self->slot, dstCh)) { PyErr_SetString(PyExc_RuntimeError, "send engage failed"); return nullptr; }
    Py_RETURN_NONE;
}
static PyObject* mcp_sendslot_disengage(MCPSendSlotObject* self, PyObject*) {
    if (s_sendDisengageCb) s_sendDisengageCb(self->ch, self->slot);
    Py_RETURN_NONE;
}
static PyMethodDef kSendSlotMethods[] = {
    {"set_pan",   (PyCFunction)mcp_sendslot_set_pan_method, METH_VARARGS, "set_pan(panL, panR)"},
    {"engage",    (PyCFunction)mcp_sendslot_engage,         METH_O,       "engage(target_channel)"},
    {"disengage", (PyCFunction)mcp_sendslot_disengage,      METH_NOARGS,  "disengage()"},
    {nullptr}
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject MCPSendSlotType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.mix_console.SendSlot", sizeof(MCPSendSlotObject), 0, nullptr,
    0, nullptr, nullptr, nullptr, (reprfunc)mcp_sendslot_repr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT, "SendSlot — send bus slot handle",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    kSendSlotMethods, nullptr, kSendSlotGetSet,
    nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    mcp_sendslot_new_guard,
};
#pragma GCC diagnostic pop

static PyObject* mcp_channel_get_send_slot(MCPChannelObject* self, PyObject* args) {
    int slot = 0; if (!PyArg_ParseTuple(args, "i", &slot)) return nullptr;
    MCPSendSlotObject* obj = (MCPSendSlotObject*)MCPSendSlotType.tp_alloc(&MCPSendSlotType, 0);
    if (!obj) return nullptr;
    obj->ch = self->ch; obj->slot = slot;
    return (PyObject*)obj;
}

// --- MCPPluginSlotObject ---

static PyObject* mcp_pluginslot_new_guard(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "PluginSlot cannot be instantiated directly");
    return nullptr;
}
static PyObject* mcp_pluginslot_repr(MCPPluginSlotObject* self) {
    return PyUnicode_FromFormat("<PluginSlot ch=%d slot=%d>", self->ch, self->slot);
}
static PyObject* mcp_pluginslot_load(MCPPluginSlotObject* self, PyObject* args) {
    const char* id = nullptr; if (!PyArg_ParseTuple(args, "s", &id)) return nullptr;
    if (!s_pluginLoadCb) { PyErr_SetString(PyExc_RuntimeError, "no plugin load callback"); return nullptr; }
    if (!s_pluginLoadCb(self->ch, self->slot, id ? id : "")) { PyErr_SetString(PyExc_RuntimeError, "plugin load failed"); return nullptr; }
    Py_RETURN_NONE;
}
static PyObject* mcp_pluginslot_unload(MCPPluginSlotObject* self, PyObject*) {
    if (s_pluginUnloadCb) s_pluginUnloadCb(self->ch, self->slot); Py_RETURN_NONE;
}
static PyObject* mcp_pluginslot_deactivate(MCPPluginSlotObject* self, PyObject*) {
    if (s_pluginDeactivateScriptletCb) s_pluginDeactivateScriptletCb(self->ch, self->slot); Py_RETURN_NONE;
}
static PyObject* mcp_pluginslot_reactivate(MCPPluginSlotObject* self, PyObject*) {
    if (s_pluginReactivateScriptletCb) s_pluginReactivateScriptletCb(self->ch, self->slot); Py_RETURN_NONE;
}
static PyObject* mcp_pluginslot_list_params(MCPPluginSlotObject* self, PyObject*) {
    if (!s_pluginListParamsCb) return PyList_New(0);
    const auto params = s_pluginListParamsCb(self->ch, self->slot);
    PyObject* result = PyList_New((Py_ssize_t)params.size()); if (!result) return nullptr;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        PyObject* d = Py_BuildValue("{s:s,s:s,s:f,s:f,s:f}",
            "id", p.id.c_str(), "name", p.name.c_str(),
            "min", p.min, "max", p.max, "current", p.current);
        if (!d) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, (Py_ssize_t)i, d);
    }
    return result;
}
static PyObject* mcp_pluginslot_get_param(MCPPluginSlotObject* self, PyObject* args) {
    const char* id = nullptr; if (!PyArg_ParseTuple(args, "s", &id)) return nullptr;
    if (!s_pluginGetParamCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_pluginGetParamCb(self->ch, self->slot, id ? id : ""));
}
static PyObject* mcp_pluginslot_set_param(MCPPluginSlotObject* self, PyObject* args) {
    const char* id = nullptr; double v = 0.0;
    if (!PyArg_ParseTuple(args, "sd", &id, &v)) return nullptr;
    if (s_pluginSetParamCb) s_pluginSetParamCb(self->ch, self->slot, id ? id : "", (float)v);
    Py_RETURN_NONE;
}
static PyObject* mcp_pluginslot_get_ch_idx  (MCPPluginSlotObject* self, void*) { return PyLong_FromLong(self->ch);   }
static PyObject* mcp_pluginslot_get_slot_idx(MCPPluginSlotObject* self, void*) { return PyLong_FromLong(self->slot); }
static PyGetSetDef kPluginSlotGetSet[] = {
    RO("ch",   mcp_pluginslot_get_ch_idx,   "channel index (0-based)"),
    RO("slot", mcp_pluginslot_get_slot_idx, "slot index (0-based)"),
    {nullptr}
};

static PyMethodDef kPluginSlotMethods[] = {
    {"load",        (PyCFunction)mcp_pluginslot_load,        METH_VARARGS, "load(plugin_id)"},
    {"unload",      (PyCFunction)mcp_pluginslot_unload,      METH_NOARGS,  "unload()"},
    {"deactivate",  (PyCFunction)mcp_pluginslot_deactivate,  METH_NOARGS,  "deactivate()"},
    {"reactivate",  (PyCFunction)mcp_pluginslot_reactivate,  METH_NOARGS,  "reactivate()"},
    {"list_params", (PyCFunction)mcp_pluginslot_list_params, METH_NOARGS,  "list_params() → [dict]"},
    {"get_param",   (PyCFunction)mcp_pluginslot_get_param,   METH_VARARGS, "get_param(id) → float"},
    {"set_param",   (PyCFunction)mcp_pluginslot_set_param,   METH_VARARGS, "set_param(id, value)"},
    {nullptr}
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject MCPPluginSlotType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "mcp.mix_console.PluginSlot", sizeof(MCPPluginSlotObject), 0, nullptr,
    0, nullptr, nullptr, nullptr, (reprfunc)mcp_pluginslot_repr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT, "PluginSlot — plugin insert slot handle",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    kPluginSlotMethods, nullptr, kPluginSlotGetSet,
    nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    mcp_pluginslot_new_guard,
};
#pragma GCC diagnostic pop

static PyObject* mcp_channel_get_plugin_slot(MCPChannelObject* self, PyObject* args) {
    int slot = 0; if (!PyArg_ParseTuple(args, "i", &slot)) return nullptr;
    MCPPluginSlotObject* obj = (MCPPluginSlotObject*)MCPPluginSlotType.tp_alloc(&MCPPluginSlotType, 0);
    if (!obj) return nullptr;
    obj->ch = self->ch; obj->slot = slot;
    return (PyObject*)obj;
}

// --- Module-level functions ---

static PyObject* py_mix_get_param(PyObject*, PyObject* args) {
    const char* path = nullptr; if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    if (!s_mixGetParamCb) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(s_mixGetParamCb(path ? path : ""));
}
static PyObject* py_mix_set_param(PyObject*, PyObject* args) {
    const char* path = nullptr; double v = 0.0;
    if (!PyArg_ParseTuple(args, "sd", &path, &v)) return nullptr;
    if (s_mixSetParamCb) s_mixSetParamCb(path ? path : "", v);
    Py_RETURN_NONE;
}
static PyObject* py_mix_list_snapshot(PyObject*, PyObject*) {
    if (!s_snapshotListCb) return PyList_New(0);
    const auto snaps = s_snapshotListCb();
    PyObject* result = PyList_New((Py_ssize_t)snaps.size()); if (!result) return nullptr;
    for (size_t i = 0; i < snaps.size(); ++i) {
        PyObject* d = Py_BuildValue("{s:i,s:s}", "id", snaps[i].id, "name", snaps[i].name.c_str());
        if (!d) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, (Py_ssize_t)i, d);
    }
    return result;
}
static PyObject* py_mix_load_snapshot(PyObject*, PyObject* args) {
    int id = 0; if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    if (!s_snapshotLoadCb) { PyErr_SetString(PyExc_RuntimeError, "no snapshot load callback"); return nullptr; }
    if (!s_snapshotLoadCb(id)) { PyErr_SetString(PyExc_RuntimeError, "snapshot load failed"); return nullptr; }
    Py_RETURN_NONE;
}
static PyObject* py_mix_save_snapshot(PyObject*, PyObject* args) {
    int id = 0; if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    if (!s_snapshotStoreCb) { PyErr_SetString(PyExc_RuntimeError, "no snapshot store callback"); return nullptr; }
    if (!s_snapshotStoreCb(id)) { PyErr_SetString(PyExc_RuntimeError, "snapshot store failed"); return nullptr; }
    Py_RETURN_NONE;
}
static PyObject* py_mix_delete_snapshot(PyObject*, PyObject* args) {
    int id = 0; if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    if (!s_snapshotDeleteCb) { PyErr_SetString(PyExc_RuntimeError, "no snapshot delete callback"); return nullptr; }
    if (!s_snapshotDeleteCb(id)) { PyErr_SetString(PyExc_RuntimeError, "snapshot delete failed"); return nullptr; }
    Py_RETURN_NONE;
}
static PyObject* py_mix_set_snapshot_scope(PyObject*, PyObject* args) {
    int id = 0; const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "is", &id, &path)) return nullptr;
    if (s_snapshotSetScopeCb) s_snapshotSetScopeCb(id, path ? path : "", true);
    Py_RETURN_NONE;
}
static PyObject* py_mix_unset_snapshot_scope(PyObject*, PyObject* args) {
    int id = 0; const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "is", &id, &path)) return nullptr;
    if (s_snapshotSetScopeCb) s_snapshotSetScopeCb(id, path ? path : "", false);
    Py_RETURN_NONE;
}
static PyObject* py_mix_check_snapshot_scope(PyObject*, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"id", "path", nullptr};
    int id = 0; const char* path = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|s", const_cast<char**>(kwlist), &id, &path)) return nullptr;
    if (!s_snapshotGetScopeCb) Py_RETURN_FALSE;
    const auto scope = s_snapshotGetScopeCb(id);
    if (!path || !*path) return PyBool_FromLong(!scope.empty());
    for (const auto& s : scope) if (s == path) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}
static PyObject* py_mix_get_channel(PyObject*, PyObject* args) {
    int ch = 0; if (!PyArg_ParseTuple(args, "i", &ch)) return nullptr;
    MCPChannelObject* obj = (MCPChannelObject*)MCPChannelType.tp_alloc(&MCPChannelType, 0);
    if (!obj) return nullptr;
    obj->ch = ch;
    return (PyObject*)obj;
}
static PyObject* py_mix_list_channels(PyObject*, PyObject*) {
    const int count = s_channelCountCb ? s_channelCountCb() : 0;
    PyObject* result = PyList_New(count); if (!result) return nullptr;
    for (int i = 0; i < count; ++i) {
        MCPChannelObject* obj = (MCPChannelObject*)MCPChannelType.tp_alloc(&MCPChannelType, 0);
        if (!obj) { Py_DECREF(result); return nullptr; }
        obj->ch = i;
        PyList_SET_ITEM(result, i, (PyObject*)obj);
    }
    return result;
}
static PyObject* py_mix_append_channel(PyObject*, PyObject*) {
    if (!s_appendChannelCb) { PyErr_SetString(PyExc_RuntimeError, "no append channel callback"); return nullptr; }
    const int ch = s_appendChannelCb();
    if (ch < 0) { PyErr_SetString(PyExc_RuntimeError, "append channel failed"); return nullptr; }
    MCPChannelObject* obj = (MCPChannelObject*)MCPChannelType.tp_alloc(&MCPChannelType, 0);
    if (!obj) return nullptr;
    obj->ch = ch;
    return (PyObject*)obj;
}
static PyObject* py_mix_remove_channel(PyObject*, PyObject* arg) {
    int ch = -1;
    if (PyLong_Check(arg)) { ch = (int)PyLong_AsLong(arg); }
    else if (PyObject_IsInstance(arg, (PyObject*)&MCPChannelType)) { ch = ((MCPChannelObject*)arg)->ch; }
    else { PyErr_SetString(PyExc_TypeError, "arg must be int or Channel"); return nullptr; }
    if (!s_removeChannelCb) { PyErr_SetString(PyExc_RuntimeError, "no remove channel callback"); return nullptr; }
    if (!s_removeChannelCb(ch)) { PyErr_SetString(PyExc_RuntimeError, "remove channel failed"); return nullptr; }
    Py_RETURN_NONE;
}

static PyMethodDef kMixConsoleMethods[] = {
    {"get_param",            py_mix_get_param,                                  METH_VARARGS,                   "get_param(path) → float"},
    {"set_param",            py_mix_set_param,                                  METH_VARARGS,                   "set_param(path, value)"},
    {"list_snapshot",        py_mix_list_snapshot,                              METH_NOARGS,                    "list_snapshot() → [dict]"},
    {"load_snapshot",        py_mix_load_snapshot,                              METH_VARARGS,                   "load_snapshot(id)"},
    {"save_snapshot",        py_mix_save_snapshot,                              METH_VARARGS,                   "save_snapshot(id)"},
    {"delete_snapshot",      py_mix_delete_snapshot,                            METH_VARARGS,                   "delete_snapshot(id)"},
    {"set_snapshot_scope",   py_mix_set_snapshot_scope,                         METH_VARARGS,                   "set_snapshot_scope(id, path)"},
    {"unset_snapshot_scope", py_mix_unset_snapshot_scope,                       METH_VARARGS,                   "unset_snapshot_scope(id, path)"},
    {"check_snapshot_scope", (PyCFunction)py_mix_check_snapshot_scope,          METH_VARARGS | METH_KEYWORDS,   "check_snapshot_scope(id, path='') → bool"},
    {"get_channel",          py_mix_get_channel,                                METH_VARARGS,                   "get_channel(id) → Channel"},
    {"list_channels",        py_mix_list_channels,                              METH_NOARGS,                    "list_channels() → [Channel]"},
    {"append_channel",       py_mix_append_channel,                             METH_NOARGS,                    "append_channel() → Channel"},
    {"remove_channel",       py_mix_remove_channel,                             METH_O,                         "remove_channel(channel)"},
    {nullptr}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyModuleDef kMixConsoleModuleDef = {
    PyModuleDef_HEAD_INIT, "mcp.mix_console", "mcp mix console API", -1, kMixConsoleMethods
};
#pragma GCC diagnostic pop

static PyObject* PyInit_mcp_mix_console() {
    if (PyType_Ready(&MCPChannelType)    < 0) return nullptr;
    if (PyType_Ready(&MCPPluginSlotType) < 0) return nullptr;
    if (PyType_Ready(&MCPSendSlotType)   < 0) return nullptr;
    PyObject* mod = PyModule_Create(&kMixConsoleModuleDef); if (!mod) return nullptr;
    Py_INCREF(&MCPChannelType);    PyModule_AddObject(mod, "Channel",    (PyObject*)&MCPChannelType);
    Py_INCREF(&MCPPluginSlotType); PyModule_AddObject(mod, "PluginSlot", (PyObject*)&MCPPluginSlotType);
    Py_INCREF(&MCPSendSlotType);   PyModule_AddObject(mod, "SendSlot",   (PyObject*)&MCPSendSlotType);
    return mod;
}

// ---------------------------------------------------------------------------
// mcp module (top-level)

static PyObject* py_go(PyObject*, PyObject*) { if (s_goCb) s_goCb(); Py_RETURN_NONE; }
static PyObject* py_select(PyObject*, PyObject* args) {
    const char* num = nullptr;
    if (!PyArg_ParseTuple(args, "s", &num)) return nullptr;
    if (s_selectCb && num && !s_selectCb(num)) {
        PyObject* exc = s_errCueNotFound ? s_errCueNotFound : PyExc_ValueError;
        PyErr_Format(exc, "no cue with number '%s'", num); return nullptr;
    }
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

static PyObject* py_file(PyObject*, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"title", "mode", "filter", nullptr};
    const char* title  = "Open File";
    const char* mode   = "open";
    const char* filter = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|sss",
            const_cast<char**>(kwlist), &title, &mode, &filter))
        return nullptr;
    if (!s_fileCb) Py_RETURN_NONE;
    auto result = s_fileCb(title ? title : "Open File",
                            mode   ? mode   : "open",
                            filter ? filter : "");
    if (!result.has_value()) Py_RETURN_NONE;
    return PyUnicode_FromString(result->c_str());
}

static PyObject* py_input(PyObject*, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"prompt", "default", "title", nullptr};
    const char* prompt     = "";
    const char* defaultVal = "";
    const char* title      = "Input";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ss",
            const_cast<char**>(kwlist), &prompt, &defaultVal, &title))
        return nullptr;
    if (!s_inputCb) Py_RETURN_NONE;
    auto result = s_inputCb(prompt     ? prompt     : "",
                             defaultVal ? defaultVal : "",
                             title      ? title      : "Input");
    if (!result.has_value()) Py_RETURN_NONE;
    return PyUnicode_FromString(result->c_str());
}

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

static PyMethodDef kMcpMethods[] = {
    {"go",        py_go,      METH_NOARGS,                    "go()"},
    {"select",    py_select,  METH_VARARGS,                   "select(num)"},
    {"alert",     py_alert,   METH_VARARGS,                   "alert(msg)"},
    {"confirm",   py_confirm, METH_VARARGS,                   "confirm(msg) → bool"},
    {"panic",     py_panic,   METH_NOARGS,                    "panic()"},
    {"file",      (PyCFunction)py_file,  METH_VARARGS | METH_KEYWORDS,
                  "file(title='Open File', mode='open', filter='') → str | None\n"
                  "mode: 'open' | 'save' | 'dir'"},
    {"input",     (PyCFunction)py_input, METH_VARARGS | METH_KEYWORDS,
                  "input(prompt, default='', title='Input') → str | None"},
    {"get_mc",    py_get_mc,  METH_NOARGS,                    "get_mc() → dict"},
    {"get_state", py_get_state, METH_NOARGS,                  "get_state() → dict"},
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

    registerSub("mcp.cue",         PyInit_mcp_cue());
    registerSub("mcp.cue_list",    PyInit_mcp_cue_list());
    registerSub("mcp.event",       PyInit_mcp_event());
    registerSub("mcp.time",        PyInit_mcp_time());
    registerSub("mcp.error",       PyInit_mcp_error());
    registerSub("mcp.mix_console", PyInit_mcp_mix_console());

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
void ScriptletEngine::setSelectCallback  (std::function<bool(const std::string&)> cb)  { s_selectCb    = std::move(cb); }
void ScriptletEngine::setAlertCallback   (std::function<void(const std::string&)> cb)  { s_alertCb     = std::move(cb); }
void ScriptletEngine::setConfirmCallback (std::function<bool(const std::string&)> cb)  { s_confirmCb   = std::move(cb); }
void ScriptletEngine::setOutputCallback  (std::function<void(const std::string&)> cb)  { s_outputCb    = std::move(cb); }
void ScriptletEngine::setPanicCallback   (std::function<void()> cb)                    { s_panicCb     = std::move(cb); }
void ScriptletEngine::setFileCallback    (std::function<std::optional<std::string>(const std::string&, const std::string&, const std::string&)> cb) { s_fileCb  = std::move(cb); }
void ScriptletEngine::setInputCallback   (std::function<std::optional<std::string>(const std::string&, const std::string&, const std::string&)> cb) { s_inputCb = std::move(cb); }

void ScriptletEngine::setCueInfoCallback   (std::function<ScriptletCueInfo(int)> cb)              { s_cueInfoCb    = std::move(cb); }
void ScriptletEngine::setCueSelectCallback (std::function<void(int)> cb)                          { s_cueSelectCb  = std::move(cb); }
void ScriptletEngine::setCueGoCallback     (std::function<void(int)> cb)                          { s_cueGoCb      = std::move(cb); }
void ScriptletEngine::setCueStartCallback  (std::function<void(int)> cb)                          { s_cueStartCb   = std::move(cb); }
void ScriptletEngine::setCueArmCallback    (std::function<void(int, double)> cb)                  { s_cueArmCb     = std::move(cb); }
void ScriptletEngine::setCueStopCallback   (std::function<void(int)> cb)                          { s_cueStopCb    = std::move(cb); }
void ScriptletEngine::setCueDisarmCallback (std::function<void(int)> cb)                          { s_cueDisarmCb  = std::move(cb); }
void ScriptletEngine::setCueSetNameCallback(std::function<void(int, const std::string&)> cb)      { s_cueSetNameCb = std::move(cb); }

void ScriptletEngine::setCueListCountCallback   (std::function<int(int)> cb)                                                                          { s_clCountCb    = std::move(cb); }
void ScriptletEngine::setCueListInfoCallback    (std::function<ScriptletCueInfo(int, int)> cb)                                                        { s_clInfoCb     = std::move(cb); }
void ScriptletEngine::setCueListInsertCallback  (std::function<int(int, const std::string&, const std::string&, const std::string&)> cb)              { s_clInsertCb   = std::move(cb); }
void ScriptletEngine::setCueListInsertAtCallback(std::function<int(int, int, const std::string&, const std::string&, const std::string&)> cb)         { s_clInsertAtCb = std::move(cb); }
void ScriptletEngine::setCueListMoveCallback    (std::function<bool(int, int, int, bool)> cb)                                                         { s_clMoveCb     = std::move(cb); }
void ScriptletEngine::setCueListDeleteCallback  (std::function<bool(int, int)> cb)                                                                    { s_clDeleteCb   = std::move(cb); }

void ScriptletEngine::setInsertCueListCallback  (std::function<int(const std::string&)> cb)       { s_insertListCb   = std::move(cb); }
void ScriptletEngine::setInsertCueListAtCallback(std::function<int(int, const std::string&)> cb)  { s_insertListAtCb = std::move(cb); }
void ScriptletEngine::setDeleteCueListCallback  (std::function<bool(int)> cb)                     { s_deleteListCb   = std::move(cb); }

void ScriptletEngine::setGetMCCallback       (std::function<ScriptletMCInfo()> cb)                { s_getMcCb            = std::move(cb); }
void ScriptletEngine::setGetStateCallback    (std::function<ScriptletStateInfo()> cb)             { s_getStateCb         = std::move(cb); }
void ScriptletEngine::setGetSampleRateCallback(std::function<int()> cb)                           { s_getSampleRateCb    = std::move(cb); }
void ScriptletEngine::setMusicalToSecondsCallback(std::function<double(int, int, int)> cb)        { s_musicalToSecondsCb = std::move(cb); }

void ScriptletEngine::setListInfoCallback    (std::function<std::vector<std::pair<int,std::string>>()> cb) { s_listInfoCb     = std::move(cb); }
void ScriptletEngine::setActiveListIdCallback(std::function<int()> cb)                                     { s_activeListIdCb = std::move(cb); }

void ScriptletEngine::setMixGetParamCallback(std::function<double(const std::string&)> cb)               { s_mixGetParamCb  = std::move(cb); }
void ScriptletEngine::setMixSetParamCallback(std::function<void(const std::string&, double)> cb)         { s_mixSetParamCb  = std::move(cb); }

void ScriptletEngine::setSnapshotListCallback  (std::function<std::vector<ScriptletSnapshotEntry>()> cb)            { s_snapshotListCb     = std::move(cb); }
void ScriptletEngine::setSnapshotLoadCallback  (std::function<bool(int)> cb)                                        { s_snapshotLoadCb     = std::move(cb); }
void ScriptletEngine::setSnapshotStoreCallback (std::function<bool(int)> cb)                                        { s_snapshotStoreCb    = std::move(cb); }
void ScriptletEngine::setSnapshotDeleteCallback(std::function<bool(int)> cb)                                        { s_snapshotDeleteCb   = std::move(cb); }
void ScriptletEngine::setSnapshotGetScopeCallback(std::function<std::vector<std::string>(int)> cb)                  { s_snapshotGetScopeCb = std::move(cb); }
void ScriptletEngine::setSnapshotSetScopeCallback(std::function<void(int, const std::string&, bool)> cb)            { s_snapshotSetScopeCb = std::move(cb); }

void ScriptletEngine::setChannelCountCallback      (std::function<int()> cb)                              { s_channelCountCb       = std::move(cb); }
void ScriptletEngine::setChannelInfoCallback       (std::function<ScriptletChannelInfo(int)> cb)          { s_channelInfoCb        = std::move(cb); }
void ScriptletEngine::setChannelSetNameCallback    (std::function<void(int, const std::string&)> cb)      { s_channelSetNameCb     = std::move(cb); }
void ScriptletEngine::setChannelSetFaderCallback   (std::function<void(int, float)> cb)                   { s_channelSetFaderCb    = std::move(cb); }
void ScriptletEngine::setChannelSetMuteCallback    (std::function<void(int, bool)> cb)                    { s_channelSetMuteCb     = std::move(cb); }
void ScriptletEngine::setChannelSetPolarityCallback(std::function<void(int, bool)> cb)                    { s_channelSetPolarityCb = std::move(cb); }
void ScriptletEngine::setChannelSetDelayCallback   (std::function<void(int, float)> cb)                   { s_channelSetDelayCb    = std::move(cb); }
void ScriptletEngine::setChannelSetPdcCallback     (std::function<void(int, bool)> cb)                    { s_channelSetPdcCb      = std::move(cb); }
void ScriptletEngine::setChannelGetXpointCallback  (std::function<float(int, int)> cb)                    { s_channelGetXpointCb   = std::move(cb); }
void ScriptletEngine::setChannelSetXpointCallback  (std::function<void(int, int, float)> cb)              { s_channelSetXpointCb   = std::move(cb); }
void ScriptletEngine::setChannelLinkCallback       (std::function<void(int, bool)> cb)                    { s_channelLinkCb        = std::move(cb); }
void ScriptletEngine::setAppendChannelCallback     (std::function<int()> cb)                              { s_appendChannelCb      = std::move(cb); }
void ScriptletEngine::setRemoveChannelCallback     (std::function<bool(int)> cb)                          { s_removeChannelCb      = std::move(cb); }

void ScriptletEngine::setPluginSlotCountCallback  (std::function<int(int)> cb)                                            { s_pluginSlotCountCb          = std::move(cb); }
void ScriptletEngine::setPluginListParamsCallback (std::function<std::vector<ScriptletPluginParamInfo>(int, int)> cb)      { s_pluginListParamsCb         = std::move(cb); }
void ScriptletEngine::setPluginGetParamCallback   (std::function<float(int, int, const std::string&)> cb)                 { s_pluginGetParamCb           = std::move(cb); }
void ScriptletEngine::setPluginSetParamCallback   (std::function<void(int, int, const std::string&, float)> cb)           { s_pluginSetParamCb           = std::move(cb); }
void ScriptletEngine::setPluginLoadCallback       (std::function<bool(int, int, const std::string&)> cb)                  { s_pluginLoadCb               = std::move(cb); }
void ScriptletEngine::setPluginUnloadCallback     (std::function<bool(int, int)> cb)                                      { s_pluginUnloadCb             = std::move(cb); }
void ScriptletEngine::setPluginDeactivateScriptletCallback(std::function<void(int, int)> cb)                              { s_pluginDeactivateScriptletCb = std::move(cb); }
void ScriptletEngine::setPluginReactivateScriptletCallback(std::function<void(int, int)> cb)                              { s_pluginReactivateScriptletCb = std::move(cb); }

void ScriptletEngine::setChannelSendCountCallback(std::function<int(int)> cb)                             { s_channelSendCountCb = std::move(cb); }
void ScriptletEngine::setSendInfoCallback        (std::function<ScriptletSendInfo(int, int)> cb)          { s_sendInfoCb         = std::move(cb); }
void ScriptletEngine::setSendSetMuteCallback     (std::function<void(int, int, bool)> cb)                 { s_sendSetMuteCb      = std::move(cb); }
void ScriptletEngine::setSendSetLevelCallback    (std::function<void(int, int, float)> cb)                { s_sendSetLevelCb     = std::move(cb); }
void ScriptletEngine::setSendSetPanCallback      (std::function<void(int, int, float, float)> cb)         { s_sendSetPanCb       = std::move(cb); }
void ScriptletEngine::setSendEngageCallback      (std::function<bool(int, int, int)> cb)                  { s_sendEngageCb       = std::move(cb); }
void ScriptletEngine::setSendDisengageCallback   (std::function<bool(int, int)> cb)                       { s_sendDisengageCb    = std::move(cb); }

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
