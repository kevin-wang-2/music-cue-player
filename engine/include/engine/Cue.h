#pragma once

#include "AudioFile.h"
#include "FadeData.h"
#include "MusicContext.h"
#include "StreamReader.h"
#include "Timecode.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

enum class CueType { Audio, Start, Stop, Fade, Arm, Devamp, Group, MusicContext, Marker, Network, Midi, Timecode, Goto, Memo, Scriptlet, Snapshot, Automation, Deactivate, Reactivate };

// Per-audio-cue channel routing.
// outLevelDb[o]  — per-output-channel level in dB (0.0 = unity).
// xpoint[s][o]   — crosspoint matrix: std::nullopt = no route (cell disabled).
//                  Default when xpoint is empty: diagonal = 0.0 dB, rest = nullopt.
struct Routing {
    std::vector<float> outLevelDb;
    std::vector<std::vector<std::optional<float>>> xpoint;  // [srcCh][outCh]
};

// Group cue execution mode.
struct GroupData {
    enum class Mode { Timeline, Playlist, StartFirst, Sync };
    Mode mode   = Mode::Timeline;
    bool random = false;   // Playlist only: randomise child order

    // Sync mode runtime state (non-serialized; reset on each fireSyncGroup call).
    int  syncGeneration{0};  // bumped to invalidate stale slice-end callbacks
    int  syncPlaySlice{0};   // which slice index is currently playing
    int  syncLoopsLeft{-1};  // -1=unset; 0=infinite; N>0=N loops remaining
    int  syncDevampMode{-1};    // -1=none; 0=next-slice; 1=go-stop; 2=go-keep
    bool syncDevampPreVamp{false}; // skip next slice if it loops
};

struct Cue {
    CueType     type{CueType::Audio};
    std::string cueNumber;
    std::string name;

    // Stable identity — assigned once by CueList, never changes on insert/remove/move.
    // Used for voice tagging so live voices survive structural list mutations.
    int stableId{-1};

    // Audio cues
    std::string path;
    AudioFile   audioFile;

    // Start/Stop/Arm/Devamp/Fade/Marker cues: index of the target cue in the same CueList
    int    targetIndex{-1};
    // Cross-list target (set by ShowHelpers for multi-list shows).
    // When crossListNumericId != -1, the cue's target lives in a different CueList.
    // CueList::fire() will invoke the cross-list callback instead of acting locally.
    int    crossListNumericId{-1};
    int    crossListFlatIdx{-1};
    // Marker cues only: which marker within the target cue (-1 = cue start)
    int    markerIndex{-1};
    // Arm cues only: pre-load the target audio from this offset (seconds). 0 = from start.
    double armStartTime{0.0};

    // Devamp cues
    int    devampMode{0};
    bool   devampPreVamp{false};

    // Timing (all types)
    double preWaitSeconds{0.0};
    // 0=none, 1=next bar, 2=next beat  (uses global MC if one is playing)
    int    goQuantize{0};

    // Playback region (Audio cues only)
    double startTime{0.0};
    double duration{0.0};

    // Gain (Audio cues only, dB; 0 = unity)
    double level{0.0};
    double trim{0.0};

    // Channel routing (Audio cues only)
    Routing routing;

    // Fade cues only — null for all other types
    std::shared_ptr<FadeData> fadeData;

    // ARM state: non-null when this audio cue has been pre-buffered for instant start.
    std::shared_ptr<StreamReader> armedStream;

    // Time markers within the audio file (audio cues only).
    struct TimeMarker {
        double      time{0.0};
        std::string name;
        // Optional anchor: when an audio cue plays through this marker, the engine
        // auto-advances selectedIndex to logicalNext(anchorMarkerCueIdx).
        // -1 = no anchor.  At most one Marker cue per TimeMarker.
        int anchorMarkerCueIdx{-1};
    };
    std::vector<TimeMarker> markers;
    std::vector<int>        sliceLoops;

    // Scriptlet cues
    std::string scriptletCode;        // Python source code to execute

    // Snapshot cues
    int snapshotId{-1};               // stable snapshot ID to recall; -1 = none

    // Deactivate / Reactivate cues — target plugin slot in the mix console
    int pluginChannel{-1};  // 0-based channel index (-1 = unset)
    int pluginSlot   {-1};  // 0-based slot index within the channel chain (-1 = unset)

    // Automation cues
    // AutomationParamMode classifies paths: Linear = smooth interpolation (fader, xp),
    // Step = snap to 0/1 (mute, polarity), Forbidden = not user-selectable (delay).
    enum class AutomationParamMode { Linear, Step, Forbidden };
    static AutomationParamMode automationParamMode(const std::string& path) {
        if (path.find("/plugin/") != std::string::npos) return AutomationParamMode::Linear;
        if (path.find("/delay") != std::string::npos) return AutomationParamMode::Forbidden;
        if (path.find("/mute")  != std::string::npos) return AutomationParamMode::Step;
        if (path.find("/polarity") != std::string::npos) return AutomationParamMode::Step;
        return AutomationParamMode::Linear;
    }
    struct AutomationPoint {
        double time{0.0};       // seconds from cue start
        double value{0.0};      // native units: dB for fader/xp, 0/1 for step params
        bool   isHandle{false}; // true = per-segment PCHIP shape handle (one per segment)
    };
    std::string                  automationPath;       // parameter path, e.g. "/mixer/0/fader"
    std::vector<AutomationPoint> automationCurve;     // breakpoints + handles interleaved: BP,H,BP,H,...,BP
    double                       automationDuration{5.0};   // total cue duration (seconds)

    // Network cues
    int         networkPatchIdx{-1};  // index into CueList's network patch list
    std::string networkCommand;       // OSC address+args, or plain text

    // MIDI cues
    int         midiPatchIdx{-1};     // index into CueList's MIDI patch list
    std::string midiMessageType;      // "note_on"|"note_off"|"program_change"|"control_change"|"pitchbend"
    int         midiChannel{1};       // 1–16
    int         midiData1{60};        // note/program/controller; pitchbend value (-8192..8191)
    int         midiData2{64};        // velocity/value (unused for program_change and pitchbend)

    // Timecode cues
    std::string tcType{"ltc"};        // "ltc" | "mtc"
    TcFps       tcFps{TcFps::Fps25};
    TcPoint     tcStartTC;            // start timecode
    TcPoint     tcEndTC;              // end timecode (exclusive)
    int         tcLtcChannel{0};      // LTC: 0-based physical output channel
    int         tcMidiPatchIdx{-1};   // MTC: index into CueList's MIDI patch list

    // Post-trigger behaviour (all types)
    bool autoContinue{false};
    bool autoFollow{false};

    // Group cue fields —————————————————————————————————————————————————
    // Non-null iff type == Group.  Owned by the cue; move-only because of unique_ptr.
    std::unique_ptr<GroupData> groupData;

    // Flat-list position of the immediate parent Group cue (-1 = top-level).
    // Set on children when the list is built from ShowFile data.
    int parentIndex{-1};

    // Total number of flat-list entries that belong to this group (all descendants,
    // not just direct children).  Group cues only; 0 for all other types.
    // Invariant: m_cues[groupIdx + 1 .. groupIdx + childCount] are all descendants.
    int childCount{0};

    // Offset of this cue within its parent Timeline group (seconds).
    // Ignored for top-level cues and for Playlist/StartFirst group children.
    double timelineOffset{0.0};

    // Arm start position for Timeline group cues (seconds).
    // When > 0, fireGroup starts the timeline from this offset instead of 0.
    double timelineArmSec{0.0};

    // Music Context — null when not attached.
    // Valid for Audio, Group (Timeline/Sync) cues only.
    std::unique_ptr<MusicContext> musicContext;

    // Index of the cue whose musicContext this cue inherits (-1 = own context).
    // When set, CueList::musicContextOf() follows this index instead of musicContext.
    // Cleared automatically by setCueMusicContext(); serialised as mcSourceNumber.
    int mcSourceIdx{-1};

    bool isLoaded() const {
        if (type == CueType::Audio)        return audioFile.isLoaded();
        if (type == CueType::Fade)         return fadeData != nullptr && targetIndex >= 0;
        if (type == CueType::Devamp)       return targetIndex >= 0;
        if (type == CueType::MusicContext) return musicContext != nullptr;
        if (type == CueType::Marker)       return targetIndex >= 0 && markerIndex >= 0;
        if (type == CueType::Network)      return networkPatchIdx >= 0;
        if (type == CueType::Midi)         return midiPatchIdx >= 0;
        if (type == CueType::Timecode)     return tcStartTC < tcEndTC;
        if (type == CueType::Goto)       return targetIndex >= 0;
        if (type == CueType::Snapshot)    return snapshotId >= 0;
        if (type == CueType::Automation)  return !automationPath.empty() && !automationCurve.empty();
        if (type == CueType::Deactivate)  return pluginChannel >= 0 && pluginSlot >= 0;
        if (type == CueType::Reactivate)  return pluginChannel >= 0 && pluginSlot >= 0;
        return true;   // Group, Start, Stop, Arm, Memo, Scriptlet always "loaded"
    }
};

} // namespace mcp
