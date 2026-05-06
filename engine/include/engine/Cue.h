#pragma once

#include "AudioFile.h"
#include "FadeData.h"
#include "StreamReader.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

enum class CueType { Audio, Start, Stop, Fade, Arm, Devamp };

// Per-audio-cue channel routing.
// outLevelDb[o]  — per-output-channel level in dB (0.0 = unity).
// xpoint[s][o]   — crosspoint matrix: std::nullopt = no route (cell disabled).
//                  Default when xpoint is empty: diagonal = 0.0 dB, rest = nullopt.
// Both are empty until explicitly initialized (empty → default diagonal routing).
struct Routing {
    std::vector<float> outLevelDb;
    std::vector<std::vector<std::optional<float>>> xpoint;  // [srcCh][outCh]
};

struct Cue {
    CueType     type{CueType::Audio};
    std::string cueNumber;   // user-visible Q number (e.g. "1", "2", "1a") — separate from array index
    std::string name;

    // Audio cues
    std::string path;
    AudioFile   audioFile;

    // Start/Stop/Arm/Devamp cues: index of the target cue in the same CueList
    int    targetIndex{-1};
    // Arm cues only: pre-load the target audio from this offset (seconds). 0 = from start.
    double armStartTime{0.0};

    // Devamp cues
    // devampMode: 0=NextSlice, 1=NextCue+StopCurrent, 2=NextCue+KeepCurrent
    int    devampMode{0};
    // After devamp: also skip any following slices/segments with loops != 1.
    bool   devampPreVamp{false};

    // Timing (all types)
    double preWaitSeconds{0.0};

    // Playback region (Audio cues only)
    double startTime{0.0};   // seconds offset into the file (0 = from start)
    double duration{0.0};    // seconds to play (0 = to end of file)

    // Gain (Audio cues only, dB; 0 = unity)
    double level{0.0};   // output fader
    double trim{0.0};    // fine trim on top of level

    // Channel routing (Audio cues only)
    Routing routing;

    // Fade cues only — null for all other types
    std::shared_ptr<FadeData> fadeData;

    // ARM state: non-null when this audio cue has been pre-buffered for instant start.
    // Protected by CueList::m_slotMutex when accessed from multiple threads.
    std::shared_ptr<StreamReader> armedStream;

    // Time markers within the audio file (audio cues only).
    // Times are absolute file positions (seconds).  Sorted by time.
    // Together with startTime / duration they define playback slices.
    struct TimeMarker {
        double      time{0.0};
        std::string name;
    };
    std::vector<TimeMarker> markers;    // sorted by time
    std::vector<int>        sliceLoops; // [i] = loop count for slice i; 0 = infinite; 1 = once
                                        // size = markers.size() + 1 (auto-maintained)

    // Post-trigger behaviour (all types)
    bool autoContinue{false};
    bool autoFollow{false};

    bool isLoaded() const {
        if (type == CueType::Audio)  return audioFile.isLoaded();
        if (type == CueType::Fade)   return fadeData != nullptr;
        if (type == CueType::Devamp) return targetIndex >= 0;
        return true;
    }
};

} // namespace mcp
