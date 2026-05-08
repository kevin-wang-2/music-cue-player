#include "engine/Timecode.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mcp {

// ---------------------------------------------------------------------------
TcRate tcRateFor(TcFps fps) {
    switch (fps) {
        case TcFps::Fps24:    return { 24, 1, 1, false };
        case TcFps::Fps25:    return { 25, 1, 1, false };
        case TcFps::Fps30Nd:  return { 30, 1, 1, false };
        case TcFps::Fps30Df:  return { 30, 1000, 1001, true  };
        case TcFps::Fps23976: return { 24, 1000, 1001, false };
        case TcFps::Fps24975: return { 25, 1000, 1001, false };
        case TcFps::Fps2997Nd:return { 30, 1000, 1001, false };
        case TcFps::Fps2997Df:return { 30, 1000, 1001, true  };
    }
    return { 30, 1, 1, false };
}

// ---------------------------------------------------------------------------
// Drop-frame constants (only used when rate.dropFrame && nomFPS == 30)
// Standard SMPTE DF for 29.97DF / 30DF (D = 2 dropped frames per minute).

static constexpr int kDFDropPerMin  = 2;
static constexpr int kFPS30         = 30;
static constexpr int kFps1Min0      = kFPS30 * 60;               // 1800: first minute of 10-min block
static constexpr int kFps1MinN      = kFPS30 * 60 - kDFDropPerMin; // 1798: subsequent minutes
static constexpr int kFps10Min      = kFPS30 * 10 * 60 - kDFDropPerMin * 9; // 17982
static constexpr int kFpsHour       = kFps10Min * 6;             // 107892

int64_t tcToFrames(const TcPoint& tc, TcFps fps) {
    const TcRate r = tcRateFor(fps);
    if (r.dropFrame) {
        // SMPTE drop-frame: N = fps*(3600h + 60m + s) + f − 2*(totalMin − totalMin/10)
        const int64_t totalMin = 60 * tc.hh + tc.mm;
        return (int64_t)r.nomFPS * (3600 * tc.hh + 60 * tc.mm + tc.ss) + tc.ff
               - kDFDropPerMin * (totalMin - totalMin / 10);
    }
    return (int64_t)r.nomFPS * (3600 * tc.hh + 60 * tc.mm + tc.ss) + tc.ff;
}

TcPoint framesToTc(int64_t N, TcFps fps) {
    const TcRate r = tcRateFor(fps);
    TcPoint tc;

    if (!r.dropFrame) {
        const int nom = r.nomFPS;
        tc.ff = static_cast<int>(N % nom);  N /= nom;
        tc.ss = static_cast<int>(N % 60);   N /= 60;
        tc.mm = static_cast<int>(N % 60);   N /= 60;
        tc.hh = static_cast<int>(N % 24);
        return tc;
    }

    // Drop-frame decomposition.
    tc.hh = static_cast<int>(N / kFpsHour);
    N %= kFpsHour;

    const int tenMin = static_cast<int>(N / kFps10Min);
    N %= kFps10Min;

    int onesMin = 0;
    if (N < kFps1Min0) {
        // In the first (non-drop) minute of this 10-minute block.
        onesMin = 0;
        tc.ss = static_cast<int>(N / r.nomFPS);
        tc.ff = static_cast<int>(N % r.nomFPS);
    } else {
        N -= kFps1Min0;
        onesMin = 1 + static_cast<int>(N / kFps1MinN);
        N %= kFps1MinN;
        // Shift by D because frames 0 and 1 are dropped at the start of each non-zero minute.
        const int adj = N + kDFDropPerMin;
        tc.ss = adj / r.nomFPS;
        tc.ff = adj % r.nomFPS;
    }
    tc.mm = tenMin * 10 + onesMin;
    return tc;
}

int64_t tcRangeSamples(const TcPoint& start, const TcPoint& end,
                       TcFps fps, int sampleRate) {
    const int64_t startF = tcToFrames(start, fps);
    const int64_t endF   = tcToFrames(end,   fps);
    const TcRate r = tcRateFor(fps);
    return (endF - startF) * (int64_t)sampleRate * r.rateDen / ((int64_t)r.nomFPS * r.rateNum);
}

int64_t bitStartSample(int64_t bitIdx, TcFps fps, int sampleRate) {
    const TcRate r = tcRateFor(fps);
    return bitIdx * (int64_t)sampleRate * r.rateDen / ((int64_t)r.nomFPS * 80 * r.rateNum);
}

int64_t bitMidSample(int64_t bitIdx, TcFps fps, int sampleRate) {
    const TcRate r = tcRateFor(fps);
    // = (2*bitIdx + 1) * SR * rateDen / (nomFPS * 80 * 2 * rateNum)
    return (2 * bitIdx + 1) * (int64_t)sampleRate * r.rateDen
           / ((int64_t)r.nomFPS * 160 * r.rateNum);
}

// ---------------------------------------------------------------------------
std::string tcToString(const TcPoint& tc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d",
                  tc.hh, tc.mm, tc.ss, tc.ff);
    return buf;
}

bool tcFromString(const std::string& s, TcPoint& tc) {
    TcPoint t;
    if (std::sscanf(s.c_str(), "%d:%d:%d:%d", &t.hh, &t.mm, &t.ss, &t.ff) == 4) {
        tc = t;
        return true;
    }
    return false;
}

std::string tcFpsToString(TcFps fps) {
    switch (fps) {
        case TcFps::Fps24:    return "24fps";
        case TcFps::Fps25:    return "25fps";
        case TcFps::Fps30Nd:  return "30fps_nd";
        case TcFps::Fps30Df:  return "30fps_df";
        case TcFps::Fps23976: return "23.976fps";
        case TcFps::Fps24975: return "24.975fps";
        case TcFps::Fps2997Nd:return "29.97fps_nd";
        case TcFps::Fps2997Df:return "29.97fps_df";
    }
    return "30fps_nd";
}

bool tcFpsFromString(const std::string& s, TcFps& fps) {
    if (s == "24fps")       { fps = TcFps::Fps24;     return true; }
    if (s == "25fps")       { fps = TcFps::Fps25;     return true; }
    if (s == "30fps_nd")    { fps = TcFps::Fps30Nd;   return true; }
    if (s == "30fps_df")    { fps = TcFps::Fps30Df;   return true; }
    if (s == "23.976fps")   { fps = TcFps::Fps23976;  return true; }
    if (s == "24.975fps")   { fps = TcFps::Fps24975;  return true; }
    if (s == "29.97fps_nd") { fps = TcFps::Fps2997Nd; return true; }
    if (s == "29.97fps_df") { fps = TcFps::Fps2997Df; return true; }
    return false;
}

int mtcFpsCode(TcFps fps) {
    switch (fps) {
        case TcFps::Fps24:    return 0;
        case TcFps::Fps23976: return 0;
        case TcFps::Fps25:    return 1;
        case TcFps::Fps24975: return 1;
        case TcFps::Fps2997Df:return 2;
        case TcFps::Fps30Df:  return 2;
        case TcFps::Fps30Nd:  return 3;
        case TcFps::Fps2997Nd:return 3;
    }
    return 3;
}

// ---------------------------------------------------------------------------
void buildLtcFrameBits(const TcPoint& tc, bool dropFrame, bool bits[80]) {
    std::memset(bits, 0, 80 * sizeof(bool));

    // Frame units (bits 0-3), Frame tens (bits 8-9)
    const int fu = tc.ff % 10, ft = tc.ff / 10;
    bits[0] = (fu >> 0) & 1; bits[1] = (fu >> 1) & 1;
    bits[2] = (fu >> 2) & 1; bits[3] = (fu >> 3) & 1;
    bits[8] = (ft >> 0) & 1; bits[9] = (ft >> 1) & 1;
    bits[10] = dropFrame ? 1 : 0;  // drop-frame flag

    // Seconds units (bits 16-19), Seconds tens (bits 24-26)
    const int su = tc.ss % 10, st = tc.ss / 10;
    bits[16] = (su >> 0) & 1; bits[17] = (su >> 1) & 1;
    bits[18] = (su >> 2) & 1; bits[19] = (su >> 3) & 1;
    bits[24] = (st >> 0) & 1; bits[25] = (st >> 1) & 1;
    bits[26] = (st >> 2) & 1;

    // Minutes units (bits 32-35), Minutes tens (bits 40-42)
    const int mu = tc.mm % 10, mt = tc.mm / 10;
    bits[32] = (mu >> 0) & 1; bits[33] = (mu >> 1) & 1;
    bits[34] = (mu >> 2) & 1; bits[35] = (mu >> 3) & 1;
    bits[40] = (mt >> 0) & 1; bits[41] = (mt >> 1) & 1;
    bits[42] = (mt >> 2) & 1;

    // Hours units (bits 48-51), Hours tens (bits 56-57)
    const int hu = tc.hh % 10, ht = tc.hh / 10;
    bits[48] = (hu >> 0) & 1; bits[49] = (hu >> 1) & 1;
    bits[50] = (hu >> 2) & 1; bits[51] = (hu >> 3) & 1;
    bits[56] = (ht >> 0) & 1; bits[57] = (ht >> 1) & 1;

    // Sync word (bits 64-79): 0011111111111101 as transmitted (bit 64 first)
    const bool sync[16] = {0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1};
    for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];

    // Biphase-mark correction bit (bit 27): make total 1-count even
    int ones = 0;
    for (int i = 0; i < 80; ++i) if (i != 27) ones += bits[i] ? 1 : 0;
    bits[27] = (ones % 2 != 0) ? 1 : 0;
}

} // namespace mcp
