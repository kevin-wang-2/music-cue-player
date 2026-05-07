#pragma once

#include "AudioFile.h"
#include "FadeData.h"
#include "MusicContext.h"
#include "StreamReader.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

enum class CueType { Audio, Start, Stop, Fade, Arm, Devamp, Group };

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

    // Audio cues
    std::string path;
    AudioFile   audioFile;

    // Start/Stop/Arm/Devamp/Fade cues: index of the target cue in the same CueList
    int    targetIndex{-1};
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
    };
    std::vector<TimeMarker> markers;
    std::vector<int>        sliceLoops;

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

    bool isLoaded() const {
        if (type == CueType::Audio)  return audioFile.isLoaded();
        if (type == CueType::Fade)   return fadeData != nullptr && targetIndex >= 0;
        if (type == CueType::Devamp) return targetIndex >= 0;
        return true;   // Group, Start, Stop, Arm always "loaded"
    }
};

} // namespace mcp
