#pragma once

#include <cstdint>
#include <string>

namespace mcp {

// Supported timecode frame rates.
enum class TcFps {
    Fps24,      // 24 fps
    Fps25,      // 25 fps
    Fps30Nd,    // 30 fps non-drop
    Fps30Df,    // 30 fps drop-frame (nominal 30, actual 29.97DF)
    Fps23976,   // 23.976 fps = 24000/1001
    Fps24975,   // 24.975 fps = 25000/1001
    Fps2997Nd,  // 29.97 fps non-drop = 30000/1001 ND
    Fps2997Df,  // 29.97 fps drop-frame = 30000/1001 DF
};

// Timecode point: hh:mm:ss:ff
struct TcPoint {
    int hh{0}, mm{0}, ss{0}, ff{0};

    bool operator==(const TcPoint& o) const { return hh==o.hh && mm==o.mm && ss==o.ss && ff==o.ff; }
    bool operator< (const TcPoint& o) const {
        if (hh != o.hh) return hh < o.hh;
        if (mm != o.mm) return mm < o.mm;
        if (ss != o.ss) return ss < o.ss;
        return ff < o.ff;
    }
    bool operator<=(const TcPoint& o) const { return !(o < *this); }
};

// Rate descriptor for arithmetic.
struct TcRate {
    int  nomFPS;   // nominal FPS (24, 25, or 30)
    int  rateNum;  // numerator (1000 for pulldown, 1 otherwise)
    int  rateDen;  // denominator (1001 for pulldown, 1 otherwise)
    bool dropFrame;
};

TcRate tcRateFor(TcFps fps);

// Convert TC point ↔ frame number (0 = 00:00:00:00).
int64_t tcToFrames (const TcPoint& tc, TcFps fps);
TcPoint framesToTc (int64_t frames,    TcFps fps);

// Total output samples for a TC range [start, end) at `sampleRate`.
int64_t tcRangeSamples(const TcPoint& start, const TcPoint& end,
                       TcFps fps, int sampleRate);

// Sample index (from cue start) for the beginning of bit `bitIdx`.
// Bit numbering is 0 = bit 0 of the first LTC frame (startTC frame).
int64_t bitStartSample(int64_t bitIdx, TcFps fps, int sampleRate);

// Sample index of the midpoint of bit `bitIdx` (used for '1' transitions in BMC).
int64_t bitMidSample(int64_t bitIdx, TcFps fps, int sampleRate);

// Serialize/parse.
std::string tcToString   (const TcPoint& tc);
bool        tcFromString (const std::string& s, TcPoint& tc);
std::string tcFpsToString(TcFps fps);
bool        tcFpsFromString(const std::string& s, TcFps& fps);

// MTC frame-rate code (used in QF message type 7):
//   0 = 24fps, 1 = 25fps, 2 = 29.97DF, 3 = 30ND
int mtcFpsCode(TcFps fps);

// Build the 80 BMC bits for one LTC frame.
// bits[0] = first bit transmitted (frame units LSB), bits[79] = last (sync word MSB).
// The biphase-mark-correction bit (bit 27) is set to make the total 1-count even.
void buildLtcFrameBits(const TcPoint& tc, bool dropFrame, bool bits[80]);

} // namespace mcp
