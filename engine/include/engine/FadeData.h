#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

// Per-fade-cue parameters and runtime ramp state.
// Stored as shared_ptr<FadeData> on the owning Cue (null for non-fade cues).
// The ramp is computed in a dedicated thread spawned at fire() time so that
// the audio and scheduler threads are never blocked by the arithmetic.
struct FadeData {
    enum class Curve {
        Linear,      // interpolate amplitude linearly (abrupt perceptual drop at ends)
        EqualPower   // interpolate dB linearly (perceptually uniform)
    };

    // --- Stored parameters (set at construction, persisted to show file) ----
    std::string targetCueNumber;          // user-visible target Q number
    int         resolvedTargetIdx{-1};    // array index in the same CueList
    std::string parameter{"level"};       // currently only "level" is supported
    double      targetValue{0.0};         // destination dB value
    // duration is stored on the owning Cue (shared with audio cue's playback region)
    Curve       curve{Curve::Linear};
    bool        stopWhenDone{false};      // clear target voices after final ramp step

    // --- Runtime (reset and recomputed each fire()) --------------------------
    // ramp[i] = dB level at step i (0 … steps-1).
    // Computed by computeRamp() in computeThread; rampReady flips to true when done.
    std::vector<double> ramp;
    std::atomic<bool>   rampReady{false};
    std::thread         computeThread;

    // Fills ramp[0..steps-1] from startValueDB to targetValue.
    // Must be called from a dedicated thread; sets rampReady when complete.
    void computeRamp(double startValueDB, int steps);

    ~FadeData();

    FadeData() = default;
    FadeData(const FadeData&) = delete;
    FadeData& operator=(const FadeData&) = delete;
};

} // namespace mcp
