#pragma once
#include <functional>
#include <string>
#include <vector>
#include <utility>

// Snapshot of a cue's properties returned by the cue-info callback.
struct ScriptletCueInfo {
    std::string number, name, type;
    double      preWait{0.0};
    bool        autoContinue{false}, autoFollow{false};
    bool        isPlaying{false}, isPending{false}, isArmed{false};
    // Audio cues
    std::string path;
    double      level{0.0}, trim{0.0}, startTime{0.0}, duration{0.0}, playhead{0.0};
    // Control cues with a target (Start/Stop/Arm/Devamp/Fade/Goto/Marker)
    int         targetIndex{-1};
    std::string targetNumber;
    // Scriptlet cues
    std::string code;
};

// Live application state returned by the getState callback.
struct ScriptletStateInfo {
    int              selectedCue{-1};   // flat index, -1 if none
    std::vector<int> runningCues;       // flat indices of cues with active voices
    int              mcMaster{-1};      // flat index of the outermost playing MC cue
};

// Live music-context state returned by the getMC callback.
struct ScriptletMCInfo {
    bool   valid{false};
    double bpm{120.0};
    int    timeSigNum{4}, timeSigDen{4};
    int    bar{1}, beat{1};
    double fraction{0.0};
};

// Wraps an embedded Python 3 interpreter for executing Scriptlet cues.
// Provides an importable 'mcp' module with go / select / alert / panic bindings,
// a 'mcp.cue' submodule, and a 'mcp.event' submodule for event subscriptions.
// Only one instance should exist per process (Python initialises globally).
class ScriptletEngine {
public:
    ScriptletEngine();
    ~ScriptletEngine();

    // --- Basic action callbacks ---
    void setGoCallback     (std::function<void()>                      cb);
    void setSelectCallback (std::function<void(const std::string&)>    cb);  // select by cue-number string
    void setAlertCallback  (std::function<void(const std::string&)>    cb);
    void setConfirmCallback(std::function<bool(const std::string&)>    cb);  // returns true if user confirmed
    void setOutputCallback (std::function<void(const std::string&)>    cb);  // stdout/stderr capture
    void setPanicCallback  (std::function<void()>                      cb);

    // --- mcp.cue read callbacks ---
    void setCueCountCallback  (std::function<int()>                           cb);
    void setCueInfoCallback   (std::function<ScriptletCueInfo(int)>           cb);
    void setCueSelectCallback (std::function<void(int)>                       cb);  // set cursor to idx
    void setCueGoCallback     (std::function<void(int)>                       cb);  // select idx then go
    void setCueArmCallback    (std::function<void(int, double)>               cb);  // arm(idx, startOverride)
    void setCueStopCallback   (std::function<void(int)>                       cb);
    void setCueDisarmCallback (std::function<void(int)>                       cb);
    void setCueSetNameCallback(std::function<void(int, const std::string&)>   cb);

    // --- mcp.cue mutation callbacks ---
    // insert_cue(type, number, name) → returns new flat index, or -1 on failure
    void setCueInsertCallback   (std::function<int(const std::string&, const std::string&, const std::string&)> cb);
    // insert_cue_at(refIdx, type, number, name) → returns new flat index, or -1 on failure
    void setCueInsertAtCallback (std::function<int(int, const std::string&, const std::string&, const std::string&)> cb);
    // move_cue_at(refIdx, cueIdx, toGroup)
    void setCueMoveCallback     (std::function<void(int, int, bool)>          cb);
    // delete_cue(cueIdx)
    void setCueDeleteCallback   (std::function<void(int)>                     cb);

    // --- mcp.cue.start callback ---
    void setCueStartCallback(std::function<void(int)> cb);

    // --- mcp.time callbacks ---
    // Returns the audio engine sample rate (Hz).
    void setGetSampleRateCallback(std::function<int()> cb);
    // (cueIdx, bar, beat) → seconds from cue start, or -1.0 if no MC.
    void setMusicalToSecondsCallback(std::function<double(int, int, int)> cb);

    // --- mcp.get_mc() callback ---
    void setGetMCCallback(std::function<ScriptletMCInfo()> cb);
    // --- mcp.get_state() callback ---
    void setGetStateCallback(std::function<ScriptletStateInfo()> cb);

    // --- multi-list callbacks ---
    // list_lists() → [(numericId, name), ...]
    void setListInfoCallback    (std::function<std::vector<std::pair<int,std::string>>()> cb);
    // get_active_list() → numericId of the currently active list
    void setActiveListIdCallback(std::function<int()>       cb);
    // switch_list(numericId) — change the active list
    void setSwitchListCallback  (std::function<void(int)>   cb);

    // --- mcp.event fire methods (called from AppModel on events) ---
    void fireCueFiredEvent   (int idx);
    void fireCueSelectedEvent(int idx);
    void fireCueInsertedEvent(int idx);
    void fireOscEvent        (const std::string& path);
    void fireMidiEvent       (int msgType, int channel, int data1, int data2);
    void fireMusicEvent      (int subdivision);  // subdivision: 1=bar,2=half,4=beat,8=eighth,16=sixteenth

    // --- Scriptlet library ---
    void setLibrary(const std::vector<std::pair<std::string,std::string>>& modules);

    // Execute Python code synchronously.  Returns "" on success or a traceback string.
    std::string run(const std::string& code);

private:
    void injectLibrary();

    bool m_initialized{false};
    std::vector<std::pair<std::string,std::string>> m_library;
};
