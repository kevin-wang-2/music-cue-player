#pragma once

#include "AudioFile.h"
#include <string>

namespace mcp {

enum class CueType { Audio, Start, Stop };

struct Cue {
    CueType     type{CueType::Audio};
    std::string cueNumber;   // user-visible Q number (e.g. "1", "2", "1a") — separate from array index
    std::string name;

    // Audio cues
    std::string path;
    AudioFile   audioFile;

    // Start/Stop cues: index of the target cue in the same CueList
    int targetIndex{-1};

    // Timing (all types)
    double preWaitSeconds{0.0};

    // Playback region (Audio cues only)
    double startTime{0.0};   // seconds offset into the file (0 = from start)
    double duration{0.0};    // seconds to play (0 = to end of file)

    // Post-trigger behaviour (all types)
    // autoContinue: immediately also trigger the next go() when this cue fires.
    //   Cascades: consecutive cues with autoContinue all fire from the same
    //   origin frame (same batch).
    bool autoContinue{false};

    // autoFollow: schedule go() for when this cue's audio finishes.
    //   For Stop cues the follow fires in the next scheduler poll (≈2 ms).
    bool autoFollow{false};

    bool isLoaded() const {
        return type != CueType::Audio || audioFile.isLoaded();
    }
};

} // namespace mcp
