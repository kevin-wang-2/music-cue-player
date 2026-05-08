// Minimal MTC diagnostic sender — two corrected test modes + dry-run print.
//
// KEY PROTOCOL FACTS:
//   • MTC sends 4 quarter-frames per video frame (NOT 8).
//   • 8 QF types (0-7) encode ONE TC label but span 2 video frames in time.
//   • Therefore frameIndex must advance by 2 after every group of 8 QFs.
//   • 30fps → 4×30 = 120 QF/sec, interval = 8.333 ms
//   • "+2 correction": receiver assembles QF0-7 over 2 frames and adds 2 to
//     get the current position.  Senders either:
//       (A) encode actual frameIndex — receiver applies +2 itself
//       (B) encode frameIndex+2     — receiver uses label as-is (pre-compensated)
//
// Startup sequence (both modes):
//   1. Send Full Frame SysEx (locate)
//   2. Wait 50 ms
//   3. Begin continuous QF stream
//
// Usage:
//   mtc_diag              test A: direct label    (receiver adds +2)
//   mtc_diag --plus2      test B: label+2         (pre-compensated)
//   mtc_diag --print      dry-run: print first 32 QF bytes, then exit
//   mtc_diag --print-n N  dry-run: print first N QF bytes
//   mtc_diag --port N     use MIDI port N (default 0 = IAC driver)

#include <RtMidi.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// 30 fps non-drop constants
static constexpr int kFps     = 30;
static constexpr int kFpsCode = 3;             // MTC rate code: 3 = 30fps ND

// QF rate: 4 per video frame, NOT 8.
static constexpr int kQfPerFrame = 4;
static constexpr int kQfPerSec   = kFps * kQfPerFrame;  // 120

// QF interval in nanoseconds: 1e9 / 120 = 8 333 333.333...
static constexpr int64_t kNsPerSec    = 1'000'000'000LL;
static constexpr int64_t kQfIntervalNs = kNsPerSec / kQfPerSec;  // 8 333 333

// Start TC: 01:00:00:00
static constexpr int kStartHH = 1, kStartMM = 0, kStartSS = 0, kStartFF = 0;
static constexpr int64_t kStartLinear =
    static_cast<int64_t>(kFps) * (3600*kStartHH + 60*kStartMM + kStartSS) + kStartFF;

// ---------------------------------------------------------------------------
static volatile bool g_quit = false;
static void onSigInt(int) { g_quit = true; }

static void linearToTc(int64_t N, int& hh, int& mm, int& ss, int& ff) {
    ff = static_cast<int>(N % kFps); N /= kFps;
    ss = static_cast<int>(N % 60);   N /= 60;
    mm = static_cast<int>(N % 60);   N /= 60;
    hh = static_cast<int>(N % 24);
}

// Encode one QF type nibble from TC components.
static unsigned char qfNibble(int qfType, int hh, int mm, int ss, int ff) {
    switch (qfType) {
        case 0: return static_cast<unsigned char>( ff        & 0x0F);
        case 1: return static_cast<unsigned char>((ff  >> 4) & 0x01);
        case 2: return static_cast<unsigned char>( ss        & 0x0F);
        case 3: return static_cast<unsigned char>((ss  >> 4) & 0x03);
        case 4: return static_cast<unsigned char>( mm        & 0x0F);
        case 5: return static_cast<unsigned char>((mm  >> 4) & 0x03);
        case 6: return static_cast<unsigned char>( hh        & 0x0F);
        case 7: return static_cast<unsigned char>(
                    (static_cast<unsigned char>(kFpsCode) << 1)
                    | ((hh >> 4) & 0x01));
        default: return 0;
    }
}

// Build Full Frame SysEx (locate) for the given TC.
static std::vector<unsigned char> fullFrame(int hh, int mm, int ss, int ff) {
    return {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        static_cast<unsigned char>((static_cast<unsigned char>(kFpsCode) << 5) | (hh & 0x1F)),
        static_cast<unsigned char>(mm & 0x3F),
        static_cast<unsigned char>(ss & 0x3F),
        static_cast<unsigned char>(ff & 0x1F),
        0xF7
    };
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool plus2     = false;
    bool printOnly = false;
    int  printN    = 32;
    int  portArg   = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--plus2")    plus2     = true;
        if (std::string(argv[i]) == "--print")    printOnly = true;
        if (std::string(argv[i]) == "--print-n" && i+1 < argc) printN = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "--port"    && i+1 < argc) portArg = std::atoi(argv[++i]);
    }

    // ------------------------------------------------------------------
    // Dry-run print mode: show exact QF bytes without any MIDI.
    // ------------------------------------------------------------------
    if (printOnly) {
        std::fprintf(stdout,
            "30ND  start=01:00:00:00  4 QF/frame  stride=2  label=frameIndex%s\n"
            "First %d QF messages:\n\n",
            plus2 ? "+2" : "  ", printN);

        for (int qfIdx = 0; qfIdx < printN; ++qfIdx) {
            // Stride: each group of 8 QFs advances the frame counter by 2.
            const int64_t frameIndex = kStartLinear + (static_cast<int64_t>(qfIdx) / 8) * 2;
            const int64_t labelFrame = plus2 ? (frameIndex + 2) : frameIndex;

            int hh, mm, ss, ff;
            linearToTc(labelFrame, hh, mm, ss, ff);

            const int qfType = qfIdx % 8;
            const unsigned char nibble  = qfNibble(qfType, hh, mm, ss, ff);
            const unsigned char msgByte = static_cast<unsigned char>((qfType << 4) | nibble);

            if (qfType == 0) {
                int fhh, fmm, fss, fff;
                linearToTc(frameIndex, fhh, fmm, fss, fff);
                std::fprintf(stdout,
                    "-- playing %02d:%02d:%02d:%02d  label %02d:%02d:%02d:%02d --\n",
                    fhh, fmm, fss, fff, hh, mm, ss, ff);
            }
            std::fprintf(stdout, "  QF%d  F1 %02X\n", qfType, (unsigned)msgByte);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Live MIDI sender.
    // ------------------------------------------------------------------
    std::signal(SIGINT, onSigInt);

    RtMidiOut mout;
    const unsigned int nPorts = mout.getPortCount();
    if (nPorts == 0) {
        std::fprintf(stderr, "No MIDI output ports found.\n");
        return 1;
    }

    std::fprintf(stderr, "Available MIDI outputs:\n");
    for (unsigned int i = 0; i < nPorts; ++i)
        std::fprintf(stderr, "  [%u] %s\n", i, mout.getPortName(i).c_str());

    const unsigned int portIdx = static_cast<unsigned int>(portArg);
    if (portIdx >= nPorts) {
        std::fprintf(stderr, "Port %u not found.\n", portIdx);
        return 1;
    }

    mout.openPort(portIdx);
    std::fprintf(stderr, "\nOpened  : %s\n", mout.getPortName(portIdx).c_str());
    std::fprintf(stderr, "Mode    : Test %s  (label = frameIndex%s)\n",
                 plus2 ? "B" : "A", plus2 ? "+2" : "  ");
    std::fprintf(stderr, "Rate    : 30fps ND  4 QF/frame  120 QF/sec  8.333ms/QF\n");
    std::fprintf(stderr, "Stride  : frameIndex += 2 per 8-QF group\n");
    std::fprintf(stderr, "Start   : 01:00:00:00\n");
    std::fprintf(stderr, "Ctrl-C to stop.\n\n");

    // Startup: Full Frame locate, then 50ms pause before QF stream.
    {
        const auto ff = fullFrame(kStartHH, kStartMM, kStartSS, kStartFF);
        std::fprintf(stderr, "Sending Full Frame locate: F0");
        for (size_t b = 1; b < ff.size(); ++b)
            std::fprintf(stderr, " %02X", (unsigned)ff[b]);
        std::fprintf(stderr, "\n");
        mout.sendMessage(&ff);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::fprintf(stderr, "Starting QF stream...\n\n");
    }

    using Clock = std::chrono::steady_clock;
    using Ns    = std::chrono::nanoseconds;

    const auto t0 = Clock::now();

    for (int64_t qfIdx = 0; !g_quit; ++qfIdx) {
        // Absolute target — no drift accumulation.
        const auto target = t0 + Ns(qfIdx * kQfIntervalNs);
        std::this_thread::sleep_until(target);

        if (g_quit) break;

        // frameIndex strides by 2 every 8 QFs.
        const int64_t frameIndex = kStartLinear + (qfIdx / 8) * 2;
        const int64_t labelFrame = plus2 ? (frameIndex + 2) : frameIndex;

        int hh, mm, ss, ff;
        linearToTc(labelFrame, hh, mm, ss, ff);

        const int qfType = static_cast<int>(qfIdx % 8);
        const unsigned char nibble  = qfNibble(qfType, hh, mm, ss, ff);
        const unsigned char msgByte = static_cast<unsigned char>((qfType << 4) | nibble);

        const std::vector<unsigned char> msg = { 0xF1, msgByte };
        mout.sendMessage(&msg);

        // Status: once per second (every 120 QFs)
        if (qfIdx % kQfPerSec == 0) {
            int phh, pmm, pss, pff;
            linearToTc(frameIndex, phh, pmm, pss, pff);
            std::fprintf(stderr, "playing %02d:%02d:%02d:%02d  label%s  qf=%lld\r",
                         phh, pmm, pss, pff,
                         plus2 ? "+2" : "  ",
                         (long long)qfIdx);
            std::fflush(stderr);
        }
    }

    std::fprintf(stderr, "\nStopped.\n");
    return 0;
}
