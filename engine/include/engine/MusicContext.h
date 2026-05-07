#pragma once

#include <vector>

namespace mcp {

// Tempo + time-signature map attached to a cue.
//
// Musical positions are expressed as (bar, beat) pairs — both 1-indexed.
// A "beat" is one denominator unit (e.g. 1/4 note in 4/4, 1/8 note in 3/8).
// BPM always means quarter-notes per minute (standard DAW convention).
//
// Coordinate systems
// ------------------
//   Musical time  — (bar, beat [, fraction]) — the primary edit coordinate.
//                   Points are stored here; changing an earlier point's tempo
//                   does NOT shift later points' bar/beat values, but DOES
//                   shift their real-time positions.
//
//   QN time       — total quarter-notes from bar 1 beat 1 (internal only).
//                   Used as a uniform intermediate for the tempo integral.
//
//   Real time     — seconds from the cue's start position.
//                   startOffsetSeconds encodes where bar 1 beat 1 falls.
//
// Points are kept sorted by (bar, beat).
// The first point is always isRamp=false and always has a time signature.
// No point may be added before the first point's musical position.
struct MusicContext {

    struct Point {
        int    bar{1};
        int    beat{1};
        double bpm{120.0};
        bool   isRamp{false};       // ramp from prev.bpm to this.bpm (never true for first point)
        bool   hasTimeSig{true};    // false = inherit from the previous point
        int    timeSigNum{4};
        int    timeSigDen{4};
    };

    std::vector<Point> points;          // sorted by (bar, beat); must not be empty
    double startOffsetSeconds{0.0};     // real-time position (from cue start) of bar 1 beat 1
    bool   applyBeforeStart{true};      // extrapolate first point's values before bar 1

    // ── Core conversions ─────────────────────────────────────────────────────

    // Convert musical position to seconds from cue start.
    // fraction ∈ [0, 1): fractional beat offset within the beat.
    // Positions before bar 1 beat 1 are handled via applyBeforeStart.
    double musicalToSeconds(int bar, int beat, double fraction = 0.0) const;

    // Convert seconds from cue start to musical position.
    struct MusPos { int bar{1}; int beat{1}; double fraction{0.0}; };
    MusPos secondsToMusical(double seconds) const;

    // ── Queries ──────────────────────────────────────────────────────────────

    struct TimeSig { int num{4}; int den{4}; };

    // Effective time signature at a musical position (from the most recent
    // hasTimeSig point at or before that position).
    TimeSig timeSigAt(int bar, int beat = 1) const;

    // Effective BPM at a musical position (accounts for ramps).
    double bpmAt(int bar, int beat, double fraction = 0.0) const;

    // True if any time-signature change point falls at beat > 1 of a bar
    // (creates an incomplete measure before it).
    bool hasIncompleteMeasure() const;

    // Bar number of the first incomplete measure, or -1 if none.
    int firstIncompleteMeasureBar() const;

    // ── Quantization ─────────────────────────────────────────────────────────

    // Next boundary of the given subdivision (in quarter-note fractions)
    // at or strictly after `seconds` from cue start.
    //   subdivision 1 → bar; 2 → half; 4 → quarter; 8 → eighth; 16; 32
    double nextQuantizationBoundary(double seconds, int subdivision) const;

    // Invalidate the compile cache — call after mutating points, bpm, etc.
    void markDirty() { m_dirty = true; }

    // Quarter-note count from bar 1 beat 1 to (bar, beat, fraction).
    // Made public so editor widgets can position points in QN space.
    double musicalToQN(int bar, int beat, double fraction = 0.0) const;

    // Inverse: bar/beat/fraction from QN count.
    // Handles negative QN (bars before bar 1).
    MusPos qnToMusical(double qn) const;

private:

    // Seconds from bar 1 beat 1 given QN count.
    double qnToMcSeconds(double qn) const;

    // QN count from seconds from bar 1 beat 1.
    double mcSecondsToQN(double mcSec) const;

    // Effective time sig at a musical position, with index hint.
    TimeSig timeSigBefore(int bar, int beat, int* hintIdx = nullptr) const;

    // QN distance between two positions under a single constant time sig.
    static double qnDelta(int bar1, int beat1,
                          int bar2, int beat2, double frac2,
                          int tsNum, int tsDen);

    // Seconds for a constant-BPM run of dqn quarter-notes.
    static double secsConstant(double bpm, double dqn);

    // Seconds for a linear-BPM ramp: BPM goes from bpm0 to bpm1 over
    // totalDqn quarter-notes, evaluated for the first dqn quarter-notes.
    static double secsRamp(double bpm0, double bpm1, double totalDqn, double dqn);

    // Inverse: QNs elapsed given seconds in a ramp segment.
    static double qnFromSecsRamp(double bpm0, double bpm1, double totalDqn, double sec);

    // Precomputed QN and MC-second positions per point (lazy, cleared on mutation).
    struct Compiled { double qn{0}; double mcSec{0}; };
    mutable std::vector<Compiled> m_compiled;
    mutable bool m_dirty{true};
    void compile() const;
    void ensureCompiled() const;
};

} // namespace mcp
