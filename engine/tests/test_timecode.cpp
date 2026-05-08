#include "engine/Timecode.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <thread>
#include <chrono>
#include <vector>

// LtcStreamReader lives in src/, not in a public include dir,
// so we include it via a relative path from the test directory.
#include "../src/LtcStreamReader.h"

using namespace mcp;

// ─── helpers ────────────────────────────────────────────────────────────────

static TcPoint tc(int hh, int mm, int ss, int ff) {
    return TcPoint{hh, mm, ss, ff};
}

// All 8 fps values for parameterised round-trip checks.
static const TcFps kAllFps[] = {
    TcFps::Fps24, TcFps::Fps25, TcFps::Fps30Nd, TcFps::Fps30Df,
    TcFps::Fps23976, TcFps::Fps24975, TcFps::Fps2997Nd, TcFps::Fps2997Df,
};

// ─── Section 1: serialisation ───────────────────────────────────────────────

TEST_CASE("tcToString / tcFromString round-trip", "[timecode]") {
    const TcPoint cases[] = {
        tc(0,0,0,0), tc(1,23,45,29), tc(0,0,0,29), tc(23,59,59,23)
    };
    for (const auto& pt : cases) {
        TcPoint out{};
        REQUIRE(tcFromString(tcToString(pt), out));
        CHECK(out == pt);
    }
}

TEST_CASE("tcFpsToString / tcFpsFromString round-trip", "[timecode]") {
    for (TcFps fps : kAllFps) {
        TcFps out{};
        REQUIRE(tcFpsFromString(tcFpsToString(fps), out));
        CHECK(out == fps);
    }
}

TEST_CASE("tcFromString rejects malformed input", "[timecode]") {
    TcPoint p{};
    CHECK_FALSE(tcFromString("", p));
    CHECK_FALSE(tcFromString("01:02:03",   p));   // missing field
    CHECK_FALSE(tcFromString("ab:cd:ef:gh", p));  // non-numeric
}

// ─── Section 2: integer-fps frame arithmetic ────────────────────────────────

TEST_CASE("tcToFrames: non-drop fps boundaries", "[timecode]") {
    // 24 fps
    CHECK(tcToFrames(tc(0,0,0,0),  TcFps::Fps24) == 0);
    CHECK(tcToFrames(tc(0,0,0,23), TcFps::Fps24) == 23);
    CHECK(tcToFrames(tc(0,0,1,0),  TcFps::Fps24) == 24);
    CHECK(tcToFrames(tc(0,1,0,0),  TcFps::Fps24) == 1440);
    CHECK(tcToFrames(tc(1,0,0,0),  TcFps::Fps24) == 86400);

    // 25 fps
    CHECK(tcToFrames(tc(0,0,0,0),  TcFps::Fps25) == 0);
    CHECK(tcToFrames(tc(0,0,1,0),  TcFps::Fps25) == 25);
    CHECK(tcToFrames(tc(0,1,0,0),  TcFps::Fps25) == 1500);
    CHECK(tcToFrames(tc(1,0,0,0),  TcFps::Fps25) == 90000);

    // 30 non-drop
    CHECK(tcToFrames(tc(0,0,0,0),  TcFps::Fps30Nd) == 0);
    CHECK(tcToFrames(tc(0,0,1,0),  TcFps::Fps30Nd) == 30);
    CHECK(tcToFrames(tc(0,1,0,0),  TcFps::Fps30Nd) == 1800);
    CHECK(tcToFrames(tc(1,0,0,0),  TcFps::Fps30Nd) == 108000);
}

TEST_CASE("tcToFrames: fractional fps scales correctly", "[timecode]") {
    // 23.976fps = 24000/1001; uses same nominal 24-frame arithmetic as 24fps.
    CHECK(tcToFrames(tc(0,0,1,0),  TcFps::Fps23976) == 24);
    CHECK(tcToFrames(tc(0,1,0,0),  TcFps::Fps23976) == 1440);

    // 29.97 ND = same nominal frame numbers as 30 ND
    CHECK(tcToFrames(tc(0,0,1,0),  TcFps::Fps2997Nd) == 30);
    CHECK(tcToFrames(tc(0,1,0,0),  TcFps::Fps2997Nd) == 1800);
}

TEST_CASE("framesToTc: round-trip for all fps modes", "[timecode]") {
    const TcPoint probe[] = {
        tc(0,0,0,0), tc(0,0,0,1), tc(0,0,1,0),
        // Use ff=2 so this probe lands on a valid frame for drop-frame fps too
        // (DF modes drop frames 0 and 1 at non-multiple-of-10 minute boundaries).
        tc(0,1,0,2), tc(1,0,0,0), tc(1,23,45,0)
    };
    for (TcFps fps : kAllFps) {
        const TcRate r = tcRateFor(fps);
        for (const auto& pt : probe) {
            if (pt.ff >= r.nomFPS) continue;  // ff too large for this fps
            const int64_t f = tcToFrames(pt, fps);
            CHECK(framesToTc(f, fps) == pt);
        }
    }
}

// ─── Section 3: drop-frame arithmetic ──────────────────────────────────────

TEST_CASE("Drop-frame: SMPTE frame count at minute boundaries", "[timecode][drop_frame]") {
    // At 29.97DF frames 00 and 01 of every non-zero, non-multiple-of-10 minute
    // are dropped, so the count runs continuously through them.

    // Last frame of minute 0: 00:00:59:29 = 1799
    CHECK(tcToFrames(tc(0,0,59,29), TcFps::Fps2997Df) == 1799);

    // First valid frame of minute 1: 00:01:00:02 = 1800 (02, not 00)
    CHECK(tcToFrames(tc(0,1,0,2), TcFps::Fps2997Df) == 1800);

    // Verify the inverse: frame 1800 maps back to 00:01:00:02
    CHECK(framesToTc(1800, TcFps::Fps2997Df) == tc(0,1,0,2));

    // Minute 2: first valid frame is 00:02:00:02 = 3598
    CHECK(tcToFrames(tc(0,2,0,2), TcFps::Fps2997Df) == 3598);
    CHECK(framesToTc(3598, TcFps::Fps2997Df) == tc(0,2,0,2));

    // Minute 10: NOT dropped (multiple of 10) → 00:10:00:00 is valid
    // kFps10Min = 17982
    CHECK(tcToFrames(tc(0,10,0,0), TcFps::Fps2997Df) == 17982);
    CHECK(framesToTc(17982, TcFps::Fps2997Df) == tc(0,10,0,0));

    // 30DF follows the exact same arithmetic as 29.97DF.
    CHECK(tcToFrames(tc(0,1,0,2),  TcFps::Fps30Df) == 1800);
    CHECK(framesToTc(1800,          TcFps::Fps30Df) == tc(0,1,0,2));
    CHECK(tcToFrames(tc(0,10,0,0), TcFps::Fps30Df) == 17982);
}

TEST_CASE("Drop-frame: one-hour frame count", "[timecode][drop_frame]") {
    // SMPTE: 29.97DF at 01:00:00:00 = 107892 frames
    CHECK(tcToFrames(tc(1,0,0,0), TcFps::Fps2997Df) == 107892);
    CHECK(framesToTc(107892,       TcFps::Fps2997Df) == tc(1,0,0,0));
}

// ─── Section 4: sample-count functions ─────────────────────────────────────

TEST_CASE("tcRangeSamples: exact counts at 48000 Hz", "[timecode]") {
    const int SR = 48000;

    // 25fps: 1 TC second = 25 nominal frames → exactly 48000 samples
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps25, SR) == SR);

    // 24fps: 1 second = 48000 samples (integer fps, same logic)
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps24, SR) == SR);

    // 30 ND: 1 second = 48000 samples
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps30Nd, SR) == SR);

    // 29.97 ND: 1 nominal second = 48000 * 1001/1000 = 48048 samples
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps2997Nd, SR) == 48048);

    // 23.976fps: 1 nominal second = 48000 * 1001/1000 = 48048 samples
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps23976, SR) == 48048);

    // 30DF must have the same physical rate as 29.97DF (30000/1001 fps),
    // NOT 30.000 fps.  One nominal second = 48048 samples, not 48000.
    // This test catches the bug where Fps30Df was mapped to {30,1,1,true}.
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps30Df, SR) == 48048);
    CHECK(tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps30Df, SR) ==
          tcRangeSamples(tc(0,0,0,0), tc(0,0,1,0), TcFps::Fps2997Df, SR));

    // Zero-length range
    CHECK(tcRangeSamples(tc(0,1,0,0), tc(0,1,0,0), TcFps::Fps25, SR) == 0);
}

TEST_CASE("bitStartSample: exact positions at 25fps / 48000 Hz", "[timecode]") {
    // At 25fps: bit duration = 48000 / (25 * 80) = 24 samples exactly.
    const int SR = 48000;
    CHECK(bitStartSample(0,  TcFps::Fps25, SR) == 0);
    CHECK(bitStartSample(1,  TcFps::Fps25, SR) == 24);
    CHECK(bitStartSample(2,  TcFps::Fps25, SR) == 48);
    CHECK(bitStartSample(80, TcFps::Fps25, SR) == 1920);  // one full frame
    CHECK(bitStartSample(160,TcFps::Fps25, SR) == 3840);  // two frames
}

TEST_CASE("bitMidSample: half-bit positions at 25fps / 48000 Hz", "[timecode]") {
    const int SR = 48000;
    // Mid of bit b = (2b+1) * SR / (25 * 160) = (2b+1) * 12
    CHECK(bitMidSample(0, TcFps::Fps25, SR) == 12);
    CHECK(bitMidSample(1, TcFps::Fps25, SR) == 36);
    // Must be strictly between bitStart(b) and bitStart(b+1)
    for (int b = 0; b < 80; ++b) {
        const int64_t mid   = bitMidSample(b,   TcFps::Fps25, SR);
        const int64_t start = bitStartSample(b,  TcFps::Fps25, SR);
        const int64_t next  = bitStartSample(b+1,TcFps::Fps25, SR);
        CHECK(mid > start);
        CHECK(mid < next);
    }
}

TEST_CASE("bitStartSample: monotonically increasing for 29.97fps", "[timecode]") {
    // At fractional fps integer arithmetic drifts per bit but must be monotone.
    const int SR = 48000;
    for (int b = 0; b < 160; ++b)
        CHECK(bitStartSample(b+1, TcFps::Fps2997Nd, SR) >
              bitStartSample(b,   TcFps::Fps2997Nd, SR));
}

// ─── Section 5: buildLtcFrameBits ──────────────────────────────────────────

TEST_CASE("buildLtcFrameBits: sync word at bits 64-79", "[timecode][ltc]") {
    bool bits[80];
    buildLtcFrameBits(tc(0,0,0,0), false, bits);

    // SMPTE sync word: 0011111111111101 (bit 64 = LSB transmitted first)
    const bool expected[16] = {0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1};
    for (int i = 0; i < 16; ++i)
        CHECK(bits[64 + i] == expected[i]);
}

TEST_CASE("buildLtcFrameBits: drop-frame flag at bit 10", "[timecode][ltc]") {
    bool bits[80];
    buildLtcFrameBits(tc(0,0,0,0), false, bits);
    CHECK(bits[10] == false);

    buildLtcFrameBits(tc(0,0,0,0), true,  bits);
    CHECK(bits[10] == true);
}

TEST_CASE("buildLtcFrameBits: BCD digit encoding", "[timecode][ltc]") {
    // TC 01:23:45:06 (frame 06 so ft=0, fu=6; seconds 45 → su=5,st=4;
    //                  minutes 23 → mu=3,mt=2; hours 01 → hu=1,ht=0)
    bool bits[80];
    buildLtcFrameBits(tc(1,23,45,6), false, bits);

    // Frame units (bits 0-3): fu=6 = 0b0110
    CHECK(bits[0] == 0); CHECK(bits[1] == 1); CHECK(bits[2] == 1); CHECK(bits[3] == 0);
    // Frame tens (bits 8-9): ft=0
    CHECK(bits[8] == 0); CHECK(bits[9] == 0);
    // Seconds units (bits 16-19): su=5 = 0b0101
    CHECK(bits[16] == 1); CHECK(bits[17] == 0); CHECK(bits[18] == 1); CHECK(bits[19] == 0);
    // Seconds tens (bits 24-26): st=4 = 0b100
    CHECK(bits[24] == 0); CHECK(bits[25] == 0); CHECK(bits[26] == 1);
    // Minutes units (bits 32-35): mu=3 = 0b0011
    CHECK(bits[32] == 1); CHECK(bits[33] == 1); CHECK(bits[34] == 0); CHECK(bits[35] == 0);
    // Minutes tens (bits 40-42): mt=2 = 0b010
    CHECK(bits[40] == 0); CHECK(bits[41] == 1); CHECK(bits[42] == 0);
    // Hours units (bits 48-51): hu=1 = 0b0001
    CHECK(bits[48] == 1); CHECK(bits[49] == 0); CHECK(bits[50] == 0); CHECK(bits[51] == 0);
    // Hours tens (bits 56-57): ht=0
    CHECK(bits[56] == 0); CHECK(bits[57] == 0);
}

TEST_CASE("buildLtcFrameBits: biphase correction makes 1-count even", "[timecode][ltc]") {
    // For any TC value, the total number of '1' bits must be even (SMPTE spec).
    const TcPoint cases[] = {
        tc(0,0,0,0), tc(1,23,45,29), tc(0,0,0,1), tc(0,1,0,0),
        tc(12,34,56,7), tc(0,10,0,0)
    };
    for (const auto& pt : cases) {
        bool bits[80];
        buildLtcFrameBits(pt, false, bits);
        int ones = 0;
        for (int i = 0; i < 80; ++i) ones += bits[i] ? 1 : 0;
        CHECK((ones % 2) == 0);

        buildLtcFrameBits(pt, true,  bits);
        ones = 0;
        for (int i = 0; i < 80; ++i) ones += bits[i] ? 1 : 0;
        CHECK((ones % 2) == 0);
    }
}

// ─── Section 6: LtcStreamReader integration ────────────────────────────────

// Helper: drain the reader completely, return all output samples.
static std::vector<float> drainReader(LtcStreamReader& r, int outCh = 1) {
    std::vector<float> out;
    constexpr int kChunk = 512;
    std::vector<float> buf(static_cast<size_t>(kChunk * outCh), 0.0f);

    // Wait for arm with timeout (2 s should be more than enough).
    for (int i = 0; i < 200 && !r.isArmed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(r.isArmed());

    while (!r.isDone()) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        const int64_t got = r.read(buf.data(), kChunk, outCh, 1.0f);
        if (got == 0 && r.isDone()) break;
        for (int64_t i = 0; i < got * outCh; ++i)
            out.push_back(buf[static_cast<size_t>(i)]);
        if (got == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

TEST_CASE("LtcStreamReader: sample count matches tcRangeSamples", "[timecode][ltc][stream]") {
    const int SR = 48000;

    // One second of 25fps LTC (exact: 48000 samples).
    const TcPoint start25 = tc(0,0,0,0);
    const TcPoint end25   = tc(0,0,1,0);
    LtcStreamReader r25(TcFps::Fps25, start25, end25, SR);
    r25.setRouting({1.0f}, {1.0f}, 1);

    const auto samples = drainReader(r25);
    CHECK((int64_t)samples.size() ==
          tcRangeSamples(start25, end25, TcFps::Fps25, SR));
}

TEST_CASE("LtcStreamReader: sample count for 29.97fps", "[timecode][ltc][stream]") {
    const int SR = 48000;

    // One TC second of 29.97 ND → 30 nominal frames → 48048 samples.
    const TcPoint start = tc(0,0,0,0);
    const TcPoint end   = tc(0,0,1,0);
    LtcStreamReader r(TcFps::Fps2997Nd, start, end, SR);
    r.setRouting({1.0f}, {1.0f}, 1);

    const auto samples = drainReader(r);
    CHECK((int64_t)samples.size() ==
          tcRangeSamples(start, end, TcFps::Fps2997Nd, SR));
}

TEST_CASE("LtcStreamReader: all output samples are +1 or -1", "[timecode][ltc][stream]") {
    // LTC is a biphase-mark signal: every sample must be exactly ±1.
    const int SR = 48000;
    LtcStreamReader r(TcFps::Fps25, tc(0,0,0,0), tc(0,0,0,2), SR);
    r.setRouting({1.0f}, {1.0f}, 1);

    const auto samples = drainReader(r);
    REQUIRE_FALSE(samples.empty());
    for (float s : samples)
        CHECK((s == 1.0f || s == -1.0f));
}

TEST_CASE("LtcStreamReader: BMC transition count matches expected 1-count", "[timecode][ltc][stream]") {
    // For each bit b: one transition at the start, plus one more if bit is '1'.
    // Total transitions in N-bit signal = N + (number of '1' bits in those N bits).
    // We test a single LTC frame (80 bits) at 25fps.
    const int SR = 48000;
    const TcPoint start = tc(0,0,0,0);
    const TcPoint end   = tc(0,0,0,1);  // exactly 1 frame

    // Build expected bits and count ones.
    bool expectedBits[80];
    buildLtcFrameBits(start, false, expectedBits);
    int expectedOnes = 0;
    for (int i = 0; i < 80; ++i) expectedOnes += expectedBits[i] ? 1 : 0;

    LtcStreamReader r(TcFps::Fps25, start, end, SR);
    r.setRouting({1.0f}, {1.0f}, 1);
    const auto samples = drainReader(r);
    REQUIRE((int64_t)samples.size() == tcRangeSamples(start, end, TcFps::Fps25, SR));

    // Count polarity transitions.
    int transitions = 0;
    for (size_t i = 1; i < samples.size(); ++i)
        if (samples[i] != samples[i-1]) ++transitions;

    // Each bit starts with a polarity transition. For 80 bits there are
    // 79 inter-bit boundaries visible as sample-to-sample changes (the
    // bit-0 start transition flips from the pre-signal state and is not
    // observable as a consecutive-sample diff). Each '1' bit also adds a
    // mid-bit transition. Total observable transitions = 79 + ones.
    CHECK(transitions == 79 + expectedOnes);
}

TEST_CASE("LtcStreamReader: isDone after full read", "[timecode][ltc][stream]") {
    const int SR = 48000;
    LtcStreamReader r(TcFps::Fps25, tc(0,0,0,0), tc(0,0,0,5), SR);
    r.setRouting({1.0f}, {1.0f}, 1);
    drainReader(r);
    CHECK(r.isDone());
}
