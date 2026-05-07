#include "engine/MusicContext.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace mcp {

// ── Static math helpers ───────────────────────────────────────────────────────

// Quarter-note distance from (bar1,beat1) to (bar2,beat2,frac2) under one time sig.
// Positive result when (bar2,beat2) > (bar1,beat1).
double MusicContext::qnDelta(int bar1, int beat1,
                              int bar2, int beat2, double frac2,
                              int tsNum, int tsDen)
{
    const double beats = static_cast<double>((bar2 - bar1) * tsNum + (beat2 - beat1)) + frac2;
    return beats * (4.0 / tsDen);
}

double MusicContext::secsConstant(double bpm, double dqn)
{
    return dqn * 60.0 / bpm;
}

// BPM is linear in QN space: bpm(q) = bpm0 + (bpm1-bpm0)*q/totalDqn
// Time = ∫₀^dqn 60/bpm(q) dq
// = 60·totalDqn/(bpm1-bpm0) · ln(bpm1/bpm0)   for dqn==totalDqn and bpm0≠bpm1
// General form (0 ≤ dqn ≤ totalDqn):
//   bpmAtDqn = bpm0 + (bpm1-bpm0)*dqn/totalDqn
//   = 60·totalDqn/(bpm1-bpm0) · ln(bpmAtDqn / bpm0)
double MusicContext::secsRamp(double bpm0, double bpm1, double totalDqn, double dqn)
{
    if (totalDqn <= 0.0 || dqn <= 0.0) return 0.0;
    const double db = bpm1 - bpm0;
    if (std::abs(db) < 1e-9) return secsConstant(bpm0, dqn);
    const double bpmAtDqn = bpm0 + db * dqn / totalDqn;
    return 60.0 * totalDqn / db * std::log(bpmAtDqn / bpm0);
}

// Inverse: q = totalDqn·bpm0/(bpm1-bpm0) · (exp(sec·(bpm1-bpm0)/(60·totalDqn)) - 1)
double MusicContext::qnFromSecsRamp(double bpm0, double bpm1, double totalDqn, double sec)
{
    if (totalDqn <= 0.0 || sec <= 0.0) return 0.0;
    const double db = bpm1 - bpm0;
    if (std::abs(db) < 1e-9) return sec * bpm0 / 60.0;
    return totalDqn * bpm0 / db * (std::exp(sec * db / (60.0 * totalDqn)) - 1.0);
}

// ── Internal helpers ──────────────────────────────────────────────────────────

// Returns the time sig effective at (bar, beat) — the most recent hasTimeSig
// point at or before that position, or the first point's ts if none precedes.
MusicContext::TimeSig MusicContext::timeSigBefore(int bar, int beat, int* hintIdx) const
{
    TimeSig ts{points.empty() ? 4 : points[0].timeSigNum,
               points.empty() ? 4 : points[0].timeSigDen};
    int found = 0;
    for (int i = 0; i < (int)points.size(); ++i) {
        const auto& p = points[i];
        if (p.bar > bar || (p.bar == bar && p.beat > beat)) break;
        if (p.hasTimeSig) { ts = {p.timeSigNum, p.timeSigDen}; found = i; }
    }
    if (hintIdx) *hintIdx = found;
    return ts;
}

// ── compile() ─────────────────────────────────────────────────────────────────

void MusicContext::compile() const
{
    const int N = (int)points.size();
    m_compiled.resize(N);
    if (N == 0) { m_dirty = false; return; }

    m_compiled[0].qn    = 0.0;
    m_compiled[0].mcSec = 0.0;

    TimeSig ts{points[0].timeSigNum, points[0].timeSigDen};

    for (int i = 1; i < N; ++i) {
        const auto& prev = points[i - 1];
        const auto& cur  = points[i];

        // QN from prev point to this point (constant time sig in this segment).
        const double dqn = qnDelta(prev.bar, prev.beat, cur.bar, cur.beat, 0.0,
                                    ts.num, ts.den);
        m_compiled[i].qn = m_compiled[i - 1].qn + dqn;

        // Seconds for this segment.
        double dsec;
        if (cur.isRamp) {
            dsec = secsRamp(prev.bpm, cur.bpm, dqn, dqn);
        } else {
            dsec = secsConstant(prev.bpm, dqn);
        }
        m_compiled[i].mcSec = m_compiled[i - 1].mcSec + dsec;

        // Update time sig for the NEXT segment (ts changes AT cur's position).
        if (cur.hasTimeSig) ts = {cur.timeSigNum, cur.timeSigDen};
    }
    m_dirty = false;
}


// ── musicalToQN ───────────────────────────────────────────────────────────────

double MusicContext::musicalToQN(int bar, int beat, double fraction) const
{
    ensureCompiled();
    if (points.empty()) return 0.0;

    // The first point is the musical origin; positions before it are negative QN.
    // Locate segment: find the last point whose (bar,beat) <= (bar,beat).
    int seg = 0;
    for (int i = 1; i < (int)points.size(); ++i) {
        if (points[i].bar > bar || (points[i].bar == bar && points[i].beat > beat)) break;
        seg = i;
    }

    const auto& p  = points[seg];
    const double segQN = m_compiled[seg].qn;

    // Effective time sig for the segment from points[seg] to the target.
    // The ts AT points[seg] applies from seg onward (updated after seg was compiled).
    TimeSig ts = (p.hasTimeSig) ? TimeSig{p.timeSigNum, p.timeSigDen}
                                 : timeSigBefore(p.bar, p.beat);

    // QN from points[seg] to target.
    const double dqn = qnDelta(p.bar, p.beat, bar, beat, fraction, ts.num, ts.den);
    return segQN + dqn;
}

// ── qnToMcSeconds ─────────────────────────────────────────────────────────────

double MusicContext::qnToMcSeconds(double qn) const
{
    ensureCompiled();
    if (points.empty()) return qn * 60.0 / 120.0;  // fallback

    // Handle positions before the first point (negative QN).
    if (qn <= m_compiled[0].qn) {
        // Extrapolate backward at first point's bpm (constant, no ramp before first point).
        const double dqn = qn - m_compiled[0].qn;  // negative
        return m_compiled[0].mcSec + secsConstant(points[0].bpm, dqn);
    }

    const int N = (int)points.size();

    // Find segment: last point whose qn <= target qn.
    int seg = 0;
    for (int i = 1; i < N; ++i) {
        if (m_compiled[i].qn > qn) break;
        seg = i;
    }

    const double segStartQN  = m_compiled[seg].qn;
    const double segStartSec = m_compiled[seg].mcSec;
    const double dqn         = qn - segStartQN;

    if (seg + 1 < N) {
        const auto& nextPt = points[seg + 1];
        if (nextPt.isRamp) {
            const double totalDqn = m_compiled[seg + 1].qn - segStartQN;
            return segStartSec + secsRamp(points[seg].bpm, nextPt.bpm, totalDqn, dqn);
        }
    }
    // Constant tempo of points[seg] beyond the last point.
    return segStartSec + secsConstant(points[seg].bpm, dqn);
}

// ── mcSecondsToQN ─────────────────────────────────────────────────────────────

double MusicContext::mcSecondsToQN(double mcSec) const
{
    ensureCompiled();
    if (points.empty()) return mcSec * 120.0 / 60.0;

    // Extrapolate before first point.
    if (mcSec <= m_compiled[0].mcSec) {
        const double ds = mcSec - m_compiled[0].mcSec;
        return m_compiled[0].qn + ds * points[0].bpm / 60.0;
    }

    const int N = (int)points.size();

    int seg = 0;
    for (int i = 1; i < N; ++i) {
        if (m_compiled[i].mcSec > mcSec) break;
        seg = i;
    }

    const double segStartSec = m_compiled[seg].mcSec;
    const double segStartQN  = m_compiled[seg].qn;
    const double ds          = mcSec - segStartSec;

    if (seg + 1 < N) {
        const auto& nextPt = points[seg + 1];
        if (nextPt.isRamp) {
            const double totalDqn = m_compiled[seg + 1].qn - segStartQN;
            return segStartQN + qnFromSecsRamp(points[seg].bpm, nextPt.bpm, totalDqn, ds);
        }
    }
    return segStartQN + ds * points[seg].bpm / 60.0;
}

// ── qnToMusical ───────────────────────────────────────────────────────────────

MusicContext::MusPos MusicContext::qnToMusical(double qn) const
{
    ensureCompiled();
    if (points.empty()) return {1, 1, 0.0};

    // Find which segment contains qn (same as qnToMcSeconds).
    const int N = (int)points.size();
    int seg = 0;
    for (int i = 1; i < N; ++i) {
        if (m_compiled[i].qn > qn) break;
        seg = i;
    }

    const auto& p   = points[seg];
    const double dqn = qn - m_compiled[seg].qn;  // residual QN within segment

    // Effective time sig for this segment.
    TimeSig ts = (p.hasTimeSig) ? TimeSig{p.timeSigNum, p.timeSigDen}
                                 : timeSigBefore(p.bar, p.beat);

    // Convert dqn back to beats of the current time sig.
    const double beatsPerQN = static_cast<double>(ts.den) / 4.0;
    double remBeats = dqn * beatsPerQN;   // fractional beats within segment

    int bar  = p.bar;
    int beat = p.beat;

    // Advance whole beats (floor division so remBeats stays in [0,1) for negative dqn too).
    const int wholeBeats = static_cast<int>(std::floor(remBeats));
    remBeats -= wholeBeats;   // now in [0, 1)

    beat += wholeBeats;
    while (beat < 1)      { beat += ts.num; --bar; }
    while (beat > ts.num) { beat -= ts.num; ++bar; }

    return {bar, beat, remBeats};
}

// ── ensureCompiled (definition after compile) ─────────────────────────────────

void MusicContext::ensureCompiled() const
{
    if (m_dirty) compile();
}

// ── Public API ─────────────────────────────────────────────────────────────────

double MusicContext::musicalToSeconds(int bar, int beat, double fraction) const
{
    const double qn    = musicalToQN(bar, beat, fraction);
    const double mcSec = qnToMcSeconds(qn);
    return startOffsetSeconds + mcSec;
}

MusicContext::MusPos MusicContext::secondsToMusical(double seconds) const
{
    const double mcSec = seconds - startOffsetSeconds;
    const double qn    = mcSecondsToQN(mcSec);
    return qnToMusical(qn);
}

MusicContext::TimeSig MusicContext::timeSigAt(int bar, int beat) const
{
    return timeSigBefore(bar, beat);
}

double MusicContext::bpmAt(int bar, int beat, double fraction) const
{
    ensureCompiled();
    if (points.empty()) return 120.0;

    const double qn = musicalToQN(bar, beat, fraction);

    // Find containing segment.
    const int N = (int)points.size();
    int seg = 0;
    for (int i = 1; i < N; ++i) {
        if (m_compiled[i].qn > qn) break;
        seg = i;
    }

    if (seg + 1 < N && points[seg + 1].isRamp) {
        const double totalDqn = m_compiled[seg + 1].qn - m_compiled[seg].qn;
        if (totalDqn > 0.0) {
            const double t = (qn - m_compiled[seg].qn) / totalDqn;
            return points[seg].bpm + (points[seg + 1].bpm - points[seg].bpm) * t;
        }
    }
    return points[seg].bpm;
}

bool MusicContext::hasIncompleteMeasure() const
{
    return firstIncompleteMeasureBar() >= 0;
}

int MusicContext::firstIncompleteMeasureBar() const
{
    // A time sig change point with beat > 1 creates an incomplete measure.
    for (const auto& p : points) {
        if (p.hasTimeSig && p.beat > 1) return p.bar;
    }
    return -1;
}

double MusicContext::nextQuantizationBoundary(double seconds, int subdivision) const
{
    if (points.empty() || subdivision <= 0) return seconds;

    ensureCompiled();

    const double mcSec   = seconds - startOffsetSeconds;
    const double qnNow   = mcSecondsToQN(mcSec);

    // subdivision is in terms of quarter notes: 1/subdivision QN per grid step.
    // subdivision 1 → bar (variable QN); 2,4,8,16,32 → fixed QN multiples of 4/subdivision.
    //
    // For subdivision == 1 (bar), grid points are bar lines — variable spacing.
    // For other subdivisions, grid step = 4.0 / subdivision QNs.
    if (subdivision == 1) {
        // Find next bar line after `seconds`.
        const auto pos = secondsToMusical(seconds);
        int nextBar = pos.bar;
        if (pos.beat > 1 || pos.fraction > 1e-9) ++nextBar;
        return musicalToSeconds(nextBar, 1, 0.0);
    }

    const double gridQN = 4.0 / subdivision;
    const double nextQN = (std::floor(qnNow / gridQN) + 1.0) * gridQN;
    return startOffsetSeconds + qnToMcSeconds(nextQN);
}

} // namespace mcp
