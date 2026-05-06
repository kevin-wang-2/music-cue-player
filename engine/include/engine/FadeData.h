#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

// One fadeable parameter target for a Fade cue.
// Only enabled entries are applied when the fade fires.
struct FadeTarget {
    bool  enabled{false};
    float targetDb{0.0f};
};

// Per-fade-cue parameters and runtime ramp state.
// Stored as shared_ptr<FadeData> on the owning Cue (null for non-fade cues).
//
// A fade cue can simultaneously fade any combination of:
//   masterLevel  — the target cue's main output level (level field)
//   outLevels[o] — the target cue's per-output-channel routing level
//
// All enabled targets share the same duration/curve and are interpolated
// in parallel from their captured start values to their respective targets.
struct FadeData {
    enum class Curve {
        Linear,      // linear amplitude interpolation
        EqualPower   // constant-power cosine crossfade
    };

    // --- Stored parameters (persisted to show file) -------------------------
    std::string targetCueNumber;
    int         resolvedTargetIdx{-1};
    Curve       curve{Curve::Linear};
    bool        stopWhenDone{false};

    FadeTarget masterLevel;             // fades the target cue's level field
    std::vector<FadeTarget> outLevels;  // [outCh] — per-output routing levels
    std::vector<std::vector<FadeTarget>> xpTargets;  // [srcCh][outCh] — crosspoint cells

    // --- Runtime (reset and recomputed each fire()) -------------------------
    // Normalized progress ramp: ramp[i] = t ∈ [0..1] at step i.
    // Curve shaping (cos/sin weights) is applied at step-execution time.
    std::vector<float> ramp;
    std::atomic<bool>  rampReady{false};
    std::thread        computeThread;

    // Start values captured at fire() time (before the thread runs).
    float masterLevelStartDb{0.0f};
    std::vector<float> outLevelStartDb;              // [outCh]
    std::vector<std::vector<float>> xpStartDb;       // [srcCh][outCh]

    // Fills ramp[0..steps-1] with linear progress values [0..1].
    void computeRamp(int steps);

    ~FadeData();
    FadeData() = default;
    FadeData(const FadeData&) = delete;
    FadeData& operator=(const FadeData&) = delete;
};

} // namespace mcp
